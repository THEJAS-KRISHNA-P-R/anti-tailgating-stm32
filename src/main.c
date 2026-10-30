/* TrustGate NON-SECURE world: clocks, RFID polling, status LEDs, UART link.
 * It can only reach the secure world through include/secure_api.h. */
#include <string.h>
#include "stm32f1xx_hal.h"
#include "board_pins.h"
#include "secure_api.h"
#include "rc522.h"
#include "ns_cmd.h"

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef o = {0}; RCC_ClkInitTypeDef c = {0};
    o.OscillatorType = RCC_OSCILLATORTYPE_HSE; o.HSEState = RCC_HSE_ON; o.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    o.PLL.PLLState = RCC_PLL_ON; o.PLL.PLLSource = RCC_PLLSOURCE_HSE; o.PLL.PLLMUL = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&o) == HAL_OK) {                    /* 8 MHz crystal x9 = 72 MHz */
        c.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
        c.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK; c.AHBCLKDivider = RCC_SYSCLK_DIV1;
        c.APB1CLKDivider = RCC_HCLK_DIV2; c.APB2CLKDivider = RCC_HCLK_DIV1;
        HAL_RCC_ClockConfig(&c, FLASH_LATENCY_2);
    } else {                                                  /* crystal missing/bad: run on internal 8 MHz RC */
        memset(&o, 0, sizeof o);
        o.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSE; o.HSEState = RCC_HSE_OFF;
        o.HSIState = RCC_HSI_ON; o.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT; o.PLL.PLLState = RCC_PLL_NONE;
        HAL_RCC_OscConfig(&o);
        c.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
        c.SYSCLKSource = RCC_SYSCLKSOURCE_HSI; c.AHBCLKDivider = RCC_SYSCLK_DIV1;
        c.APB1CLKDivider = RCC_HCLK_DIV1; c.APB2CLKDivider = RCC_HCLK_DIV1;
        HAL_RCC_ClockConfig(&c, FLASH_LATENCY_0);
    }
}

static void led_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE(); __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_OUTPUT_PP; g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = LED_GREEN_PIN | LED_RED_PIN; HAL_GPIO_Init(LED_GREEN_PORT, &g);
    g.Pin = LED_BOARD_PIN; HAL_GPIO_WritePin(LED_BOARD_PORT, LED_BOARD_PIN, GPIO_PIN_SET); HAL_GPIO_Init(LED_BOARD_PORT, &g);
}

typedef enum { CARD_SCAN, CARD_HOLD } card_state_t;
static card_state_t cs = CARD_SCAN;
static uint8_t last_uid[4], miss;
static uint32_t t_card, red_until, t_rc;
static bool rc_ok;
static tg_decision_t d;
static IWDG_HandleTypeDef hiwdg;

/* Poll the reader. A card is processed once per presentation: after reading we HALT it and keep
 * waking it with WUPA; only when it has been gone for ~0.5 s (or another UID shows up) do we accept a new tap. */
static void card_task(uint32_t now)
{
    if ((int32_t)(now - t_rc) >= 0) {       /* the RC522 resets on a supply dip and a loose wire shows up late: re-init instead of staying deaf until a power cycle */
        t_rc = now + (rc_ok ? 1000u : 2000u);
        if (!rc522_alive()) {
            bool was = rc_ok; rc_ok = rc522_init(); cs = CARD_SCAN; miss = 0;
            if (rc_ok != was) ns_printf(rc_ok ? "WARN,RC522_RECOVERED\r\n" : "WARN,RC522_LOST\r\n");
        }
    }
    if (!rc_ok || (int32_t)(now - t_card) < 0) return;
    t_card = now + (cs == CARD_SCAN ? 60u : 100u);
    if (rc522_wupa()) {
        uint8_t uid[4];
        if (rc522_read_uid4(uid)) {
            bool fresh = (cs == CARD_SCAN) || memcmp(uid, last_uid, 4) != 0;
            rc522_halt(); miss = 0;
            if (fresh) {
                memcpy(last_uid, uid, 4); cs = CARD_HOLD;
                d = secure_authorize_entry(uid);
                if (d == TG_GRANT) {
                    pulse_servo_once();  /* servo turns once on successful tap */
                }
                if (d != TG_GRANT) red_until = now + 600u;
            }
        } else if (cs == CARD_SCAN) {
            ns_printf("WARN,UNREADABLE_OR_UNSUPPORTED_CARD\r\n"); memset(last_uid, 0, 4); cs = CARD_HOLD; miss = 0;
        } else miss = 0;
    } else if (cs == CARD_HOLD && ++miss >= 5) cs = CARD_SCAN;
}

static void led_task(uint32_t now)
{
    static uint32_t t_led, t_hb;
    if ((int32_t)(now - t_led) >= 0) {
        t_led = now + 50u;
        tg_status_t s; get_access_status(&s);
        bool red = s.alarm_active || s.lockout_remaining_s || s.tamper_lockdown || (int32_t)(red_until - now) > 0;
        HAL_GPIO_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, s.door_unlocked ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(LED_RED_PORT, LED_RED_PIN, red ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    if ((int32_t)(now - t_hb) >= 0) { t_hb = now + 500u; HAL_GPIO_TogglePin(LED_BOARD_PORT, LED_BOARD_PIN); }
}

/* Pulse the servo once: unlock (1700 us) -> hold 500ms -> lock (700 us).
   Gives tactile feedback when an RFID card is tapped. */
static void pulse_servo_once(void)
{
    hw_servo_set_us(SERVO_UNLOCK_US_DEFAULT);
    HAL_Delay(500);
    hw_servo_set_us(SERVO_LOCK_US_DEFAULT);
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    led_init();
    ns_uart_init();
    rc_ok = rc522_init();
    uint8_t ver = rc522_version();
    secure_init();
    ns_boot_banner(rc_ok, ver);
    hiwdg.Instance = IWDG; hiwdg.Init.Prescaler = IWDG_PRESCALER_32; hiwdg.Init.Reload = 1250;   /* ~1 s on the 40 kHz LSI */
    HAL_IWDG_Init(&hiwdg);
    t_rc = HAL_GetTick() + 1000u;
    uint32_t last_tick = 0;
    for (;;) {
        uint32_t now = HAL_GetTick();
        /* fed only while the SysTick (and so the secure tick) is alive: a hang of either loop resets into the locked state */
        if (now != last_tick) { last_tick = now; HAL_IWDG_Refresh(&hiwdg); }
        card_task(now);
        led_task(now);
        ns_poll();
    }
}
