# RC522 RFID Diagnostics & Verification Report

**Project:** TrustGate STM32 Anti-Tailgating Access Control  
**Target Hardware:** STM32F103C8T6 (ARM Cortex-M3 "Blue Pill")  
**Framework:** STM32CubeF1 HAL (Bare-Metal C11, GNU Make, `arm-none-eabi-gcc`)  
**Date:** 30 October 2026  

---

## 1. Executive Summary

During live hardware bring-up, the laptop dashboard established successful bidirectional UART communication with the STM32 board on `COM5` (115200 8N1). 

- **SPI bus communication with the RC522 module is fully functional.** The board successfully initialized the chip and read back a valid silicon version register:
  ```text
  BOOT,TrustGate,1.0.0,rc522=ok,ver=0x82,sysclk=72000000
  ```
- **The symptom:** Physical presentation of the RFID card/tag against the reader does not register any tap events or increment the tap counter (`taps: 0`).
- **Conclusion:** The issue is **not** an SPI wiring error, prescaler misconfiguration, or firmware lockup. The primary suspect is an RF carrier frequency mismatch (e.g. attempting to read a 125 kHz card on a 13.56 MHz reader) or an unsupported 7-byte cascade UID tag.

---

## 2. SPI Configuration & Code Verification Matrix

We checked our firmware driver (`src/rc522.c`, `include/board_pins.h`) against all standard STM32/RC522 troubleshooting checkpoints:

| Checkpoint | Recommended Standard | TrustGate Firmware Implementation | Result / Status |
|---|---|---|:---:|
| **1. SPI Clock Prescaler** | 1 MHz to 4 MHz max | `SPI_BAUDRATEPRESCALER_32` ($\frac{72\text{ MHz}}{32} = \mathbf{2.25\text{ MHz}}$) | **MATCH / PASS** |
| **2. Chip Select (NSS/SDA)** | Manual Software GPIO output, High Speed | `PA4` configured as `GPIO_MODE_OUTPUT_PP`, High Speed; driven via `NSS_LOW()` / `NSS_HIGH()` macros | **MATCH / PASS** |
| **3. SPI Mode & Polarity** | Motorola, 8-bit, MSB first, CPOL=0, CPHA=0 | `SPI_POLARITY_LOW`, `SPI_PHASE_1EDGE`, `SPI_FIRSTBIT_MSB`, `SPI_DATASIZE_8BIT` | **MATCH / PASS** |
| **4. Version Register Check** | Must NOT be `0x00` or `0xFF` | Hardware returned `ver=0x82` via `rd(VersionReg)` at boot | **MATCH / PASS** |
| **5. Antenna Driver State** | TxControlReg bits 0 & 1 must be set (RF on) | `wr(TxControlReg, v \| 3)` explicitly executed during `rc522_init()` | **MATCH / PASS** |
| **6. Receiver Gain** | High sensitivity | `wr(RFCfgReg, 0x70)` set to maximum receiver gain (48 dB) | **MATCH / PASS** |

---

## 3. Hardware Pinout Reference (Verified)

| RC522 Module Pin | Wire Type | STM32 Blue Pill Pin | Description / Function |
|:---:|:---:|:---:|:---|
| **VCC** | Power | **3.3V** | Strictly 3.3V rail. Connecting to 5V permanently damages the RC522 IC. |
| **RST** | Control | **PB0** | Hardware reset line (Port B, pin 0). Driven HIGH during operation. |
| **GND** | Ground | **GND** | Shared common ground rail with STM32 and external servo PSU. |
| **IRQ** | Interrupt | *Unconnected* | Not used by firmware polling loop. |
| **MISO** | SPI Data In | **PA6** | SPI1 MISO (`GPIO_MODE_INPUT`, No pull). |
| **MOSI** | SPI Data Out | **PA7** | SPI1 MOSI (`GPIO_MODE_AF_PP`, High Speed). |
| **SCK** | SPI Clock | **PA5** | SPI1 Clock output (`GPIO_MODE_AF_PP`, High Speed). |
| **SDA (SS)** | Chip Select | **PA4** | Software-controlled NSS (`GPIO_MODE_OUTPUT_PP`). Active LOW during transfers. |

---

## 4. Root-Cause Analysis: Why the Tag Is Not Detected

Given that `ver=0x82` proves the STM32 can talk to the RC522 over SPI, the root cause lies in the **RF air interface** between the tag and the RC522 antenna coil:

### Cause A: 125 kHz vs. 13.56 MHz Card Frequency Mismatch (Most Probable)
- **RC522 capability:** The MFRC522 IC is strictly a **13.56 MHz High-Frequency (HF)** transceiver (ISO/IEC 14443 Type A).
- **Common pitfall:** Many RFID starter kits, student ID cards, building access cards, and generic blue keyfobs operate on **125 kHz Low-Frequency (LF)** (e.g. EM4100, TK4100, T5577).
- 125 kHz tags look visually identical to 13.56 MHz tags (same credit-card plastic or blue teardrop shape), but:
  - 125 kHz cards contain thick copper wire coils and will **never** energize or modulate an RC522 13.56 MHz RF field.
  - No software change can make an RC522 read a 125 kHz card.

### Cause B: 7-Byte UID / Cascade Tag (NTAG / Ultralight / Transit Cards)
In `src/rc522.c`, the anti-collision routine handles standard 4-byte single-size UIDs (Mifare Classic 1K):
```c
if (r[0] == 0x88) return false; /* cascade tag: 7-byte UID, unsupported */
```
If the card in use is a modern NFC tag (NTAG213/215/216, Mifare Ultralight, or many metro/transit cards), the chip sends a cascade byte (`0x88`) indicating a 7-byte UID. The current driver intentionally rejects cascade tags to adhere to the 4-byte credential schema.

### Cause C: Antenna Matching / Defective Clone Board Coil
Low-cost RC522 breakout boards from third-party manufacturers occasionally have defective ceramic capacitors ($C_1, C_2, C_{ext}$) in the analog resonant tank circuit (the inductors and capacitors between pins TX1/TX2 and the PCB trace antenna). If the tank circuit is out of resonance, the carrier wave will be too weak to induce operating voltage into the passive tag's coil.

---

## 5. Immediate Diagnostic Steps to Isolate the Cause

1. **NFC Smartphone Diagnostic (Zero Cost)**:
   - Turn **ON** NFC on an Android or iPhone.
   - Rest the back of the phone directly flat against the white circular pattern on the RC522 antenna.
   - Smartphones with NFC operate on **13.56 MHz** and emulate standard ISO 14443A tags.
   - If the dashboard immediately emits `WARN,UNREADABLE_OR_UNSUPPORTED_CARD` or an event, the RC522 RF transmitter is 100% healthy, proving that the card under test is a 125 kHz incompatible card.

2. **Verify Card Frequency Using a Smartphone**:
   - Install a free NFC tool app (such as *NFC Tools* by wakdev) on an NFC-equipped smartphone.
   - Tap the test card to the back of the phone:
     - **Phone reads it:** The card is 13.56 MHz. (Note the "Technologies" and "Size of UID").
     - **Phone does not react at all:** The card is almost certainly **125 kHz** and cannot be read by an RC522.

3. **Check Physical Orientation & Distance**:
   - The RC522 read range is small (typically 1.0 to 3.0 cm).
   - Place the tag directly against the PCB surface for at least 1 full second.

