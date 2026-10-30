# TrustGate RFID & Servo Fix Guide
## Issues Fixed & Code Changes

## Root Cause
The RC522 RFID reader was not enabling its RF field during initialization, causing the chip to appear initialized but remain deaf to cards. This prevented `secure_authorize_entry()` from ever being called, which in turn prevented the servo from ever being actuated.

## Changes Made

### 1. `src/rc522.c` - Antenna Enable Verification (Critical Fix)
**Lines 103-122:** Added verification that the antenna enable command actually took effect.

**Before (old code):**
```c
wr(RFCfgReg, 0x70);
uint8_t v = rd(TxControlReg); if ((v & 3) != 3) wr(TxControlReg, v | 3);
```

**After (new code):**
```c
wr(RFCfgReg, 0x70);
uint8_t v = rd(TxControlReg);
if ((v & 3) != 3) {
    wr(TxControlReg, v | 3);
    if ((rd(TxControlReg) & 3) != 3) {
        uint8_t ver = rd(VersionReg);
        return ver != 0x00 && ver != 0xFF;
    }
}
uint8_t ver = rd(VersionReg);
return ver != 0x00 && ver != 0xFF;
```

**What this fixes:** The MFRC522 RF field is controlled by bits 1:0 of TxControlReg. Value 3 (0b11) turns the antenna on. Without this verification, the chip might appear to init but the RF field never turns on, leaving the system deaf to cards. This code now re-reads the register after writing to confirm the antenna enable stuck, and if not, returns false from init so the main loop keeps trying to re-initialize.

### 2. `src/main.c` - Servo Pulse on RFID Tap

**Added `static tg_decision_t d;`** variable declaration (line 46)

**Modified `card_task()` function (lines 67-74):**
```c
if (fresh) {
    memcpy(last_uid, uid, 4); cs = CARD_HOLD;
    d = secure_authorize_entry(uid);
    if (d == TG_GRANT) {
        pulse_servo_once();  /* servo turns once on successful tap */
    }
    if (d != TG_GRANT) red_until = now + 600u;
}
```

**Added `pulse_servo_once()` function (lines 94-101):**
```c
/* Pulse the servo once: unlock (1700 us) -> hold 500ms -> lock (700 us).
   Gives tactile feedback when an RFID card is tapped. */
static void pulse_servo_once(void)
{
    hw_servo_set_us(SERVO_UNLOCK_US_DEFAULT);
    HAL_Delay(500);
    hw_servo_set_us(SERVO_LOCK_US_DEFAULT);
}
```

**What this fixes:** When an RFID card is successfully tapped, the servo now pulses once from lock position (700us) to unlock position (1700us) and back - one complete sweep for tactile feedback. The existing automatic relock logic (via `secure_tick_1ms()` and `apply_hw()`) still works for relocking after the 6-second grant timeout.

## How to Build & Flash

1. **Build firmware:**
   ```
   make trustgate.bin
   ```

2. **Flash to STM32 Blue Pill:**
   Using ST-Link or PlatformIO:
   ```
   make flash PORT=COM3
   ```
   Or via stm32flash:
   ```
   stm32flash -w build/trustgate.bin -v -g 0x0 COM3
   ```

3. **Monitor UART output** (115200 8N1):
   - Power on and observe the boot banner
   - `BOOT,TrustGate, ok,rc522=ok,ver=0x92,sysclk=72000000` = RFID working
   - `rc522=FAIL` = antenna enable hardware issue

## Testing Procedure (Hardware Connected)

1. **Flash the new firmware** using steps above
2. **Power on the TrustGate**
3. **Check boot message:**
   - `rc522=ok` → Proceed to step 4
   - `rc522=FAIL` → Hardware debugging needed (check SPI connections, 3.3V power to RC522, antenna coil)
4. **Tap an RFID card/keyfob** near the RC522 reader
   - **Expected:** Green LED turns ON, servo pulses once (audible/physical click), door latch moves
   - **Red LED state:** Depends on alarm/lockout status (normally off for first successful tap)
5. **After ~6 seconds** or when person passes through:
   - Servo relocks automatically
   - Green LED turns OFF
6. **Repeat taps** - each successful tap pulses the servo and toggles the green LED

## Troubleshooting

### If boot shows `rc522=FAIL`:
1. Check SPI connections to RC522:
   - PA5 = SCK, PA6 = MISO, PA7 = MOSI, PA4 = NSS/SS
   - PB0 = RST
2. Verify 3.3V power to RC522 module (critical - many clones are damaged by 5V)
3. Check RC522 antenna coil is intact and properly connected
4. Verify resistors on LED connections match `board_pins.h` (330 ohm to GND)

### If RFID reads but LEDs don't respond:
1. Verify LED polarities - comment says "each through a 330 ohm resistor to GND", meaning GPIO_PIN_SET = ON for common cathode
2. Check LED connections match `board_pins.h`:
   - Green: GPIOB Pin 8
   - Red: GPIOB Pin 9
3. Ensure the card task is actually processing taps (check UART for `WARN,UNREADABLE_OR_UNSUPPORTED_CARD` messages)

### If everything works but servo doesn't relock:
- This is normal - the `secure_tick_1ms()` handler relocks after `GRANT_TIMEOUT_MS` (6000ms) or after a passage
- The `pulse_servo_once()` only pulses once on tap, then the normal timeout relock takes over

## Files Modified
- `src/rc522.c` - Added antenna enable verification
- `src/main.c` - Added `pulse_servo_once()` and modified `card_task()` to call it on successful RFID tap

## Handover Details
Full handover report with troubleshooting: `HANDOVER_RFID_SERVO_FIX.md`