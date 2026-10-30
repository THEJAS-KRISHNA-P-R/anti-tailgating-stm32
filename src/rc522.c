#include <string.h>
#include "rc522.h"
#include "board_pins.h"

static SPI_HandleTypeDef hspi1;

enum { CommandReg = 0x01, ComIrqReg = 0x04, DivIrqReg = 0x05, ErrorReg = 0x06, FIFODataReg = 0x09, FIFOLevelReg = 0x0A,
       ControlReg = 0x0C, BitFramingReg = 0x0D, CollReg = 0x0E, ModeReg = 0x11, TxControlReg = 0x14, TxASKReg = 0x15,
       CRCResultRegH = 0x21, CRCResultRegL = 0x22, RFCfgReg = 0x26, TModeReg = 0x2A, TPrescalerReg = 0x2B,
       TReloadRegH = 0x2C, TReloadRegL = 0x2D, VersionReg = 0x37 };
enum { CMD_IDLE = 0x00, CMD_CALCCRC = 0x03, CMD_TRANSCEIVE = 0x0C, CMD_SOFTRESET = 0x0F };

#define NSS_LOW()  HAL_GPIO_WritePin(RC522_NSS_PORT, RC522_NSS_PIN, GPIO_PIN_RESET)
#define NSS_HIGH() HAL_GPIO_WritePin(RC522_NSS_PORT, RC522_NSS_PIN, GPIO_PIN_SET)

static void wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { (uint8_t)((reg << 1) & 0x7E), v };
    NSS_LOW(); HAL_SPI_Transmit(&hspi1, b, 2, 10); NSS_HIGH();
}
static uint8_t rd(uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0 }, rx[2] = {0, 0};
    NSS_LOW(); HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2, 10); NSS_HIGH();
    return rx[1];
}
static void setmask(uint8_t reg, uint8_t m) { wr(reg, rd(reg) | m); }
static void clrmask(uint8_t reg, uint8_t m) { wr(reg, rd(reg) & (uint8_t)~m); }

void HAL_SPI_MspInit(SPI_HandleTypeDef *h)
{
    if (h->Instance != SPI1) return;
    __HAL_RCC_SPI1_CLK_ENABLE(); __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_5 | GPIO_PIN_7; g.Mode = GPIO_MODE_AF_PP; g.Speed = GPIO_SPEED_FREQ_HIGH;   /* SCK, MOSI */
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_6; g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_NOPULL;                          /* MISO */
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = RC522_NSS_PIN; g.Mode = GPIO_MODE_OUTPUT_PP; g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_WritePin(RC522_NSS_PORT, RC522_NSS_PIN, GPIO_PIN_SET);
    HAL_GPIO_Init(RC522_NSS_PORT, &g);
    g.Pin = RC522_RST_PIN;
    HAL_GPIO_WritePin(RC522_RST_PORT, RC522_RST_PIN, GPIO_PIN_SET);
    HAL_GPIO_Init(RC522_RST_PORT, &g);
}

/* returns 0 ok, -1 timeout, -2 protocol/parity/overflow error, -3 collision */
static int xcvr(const uint8_t *tx, uint8_t txlen, uint8_t *rx, uint8_t *rxlen, uint8_t txbits)
{
    wr(CommandReg, CMD_IDLE); wr(ComIrqReg, 0x7F); wr(FIFOLevelReg, 0x80);
    for (uint8_t i = 0; i < txlen; i++) wr(FIFODataReg, tx[i]);
    wr(BitFramingReg, txbits & 7u);
    wr(CommandReg, CMD_TRANSCEIVE);
    setmask(BitFramingReg, 0x80);                         /* StartSend */
    uint32_t t0 = HAL_GetTick();
    for (;;) {
        uint8_t n = rd(ComIrqReg);
        if (n & 0x30) break;                              /* RxIRq | IdleIRq */
        if (n & 0x01) return -1;                          /* TimerIRq: no answer */
        if (HAL_GetTick() - t0 > 40) return -1;
    }
    uint8_t err = rd(ErrorReg);
    if (err & 0x13) return -2;                            /* BufferOvfl | ParityErr | ProtocolErr */
    uint8_t n = rd(FIFOLevelReg);
    if (rxlen) { if (n > *rxlen) n = *rxlen; for (uint8_t i = 0; i < n; i++) rx[i] = rd(FIFODataReg); *rxlen = n; }
    if (err & 0x08) return -3;
    return 0;
}

static bool crc_a(const uint8_t *d, uint8_t len, uint8_t out[2])
{
    wr(CommandReg, CMD_IDLE); wr(DivIrqReg, 0x04); wr(FIFOLevelReg, 0x80);
    for (uint8_t i = 0; i < len; i++) wr(FIFODataReg, d[i]);
    wr(CommandReg, CMD_CALCCRC);
    uint32_t t0 = HAL_GetTick();
    while (!(rd(DivIrqReg) & 0x04)) if (HAL_GetTick() - t0 > 10) return false;
    wr(CommandReg, CMD_IDLE);
    out[0] = rd(CRCResultRegL); out[1] = rd(CRCResultRegH);
    return true;
}

uint8_t rc522_version(void) { return rd(VersionReg); }
bool rc522_alive(void) { uint8_t v = rd(VersionReg); return v != 0x00 && v != 0xFF && (rd(TxControlReg) & 3) == 3; }

bool rc522_init(void)
{
    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER; hspi1.Init.Direction = SPI_DIRECTION_2LINES; hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW; hspi1.Init.CLKPhase = SPI_PHASE_1EDGE; hspi1.Init.NSS = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;       /* 2.25 MHz at 72 MHz */
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB; hspi1.Init.TIMode = SPI_TIMODE_DISABLE; hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) return false;

    HAL_GPIO_WritePin(RC522_RST_PORT, RC522_RST_PIN, GPIO_PIN_RESET); HAL_Delay(5);
    HAL_GPIO_WritePin(RC522_RST_PORT, RC522_RST_PIN, GPIO_PIN_SET);   HAL_Delay(50);
    wr(CommandReg, CMD_SOFTRESET);
    uint32_t t0 = HAL_GetTick();
    while (rd(CommandReg) & 0x10) if (HAL_GetTick() - t0 > 100) break;

    wr(TModeReg, 0x80); wr(TPrescalerReg, 0xA9); wr(TReloadRegH, 0x01); wr(TReloadRegL, 0x90);   /* ~10 ms answer timeout */
    wr(TxASKReg, 0x40);                                   /* 100% ASK */
    wr(ModeReg, 0x3D);                                    /* CRC preset 0x6363 */
    wr(RFCfgReg, 0x70);                                   /* max receiver gain (48 dB) */

/* Force antenna ON and verify it took effect.
   The MFRC522 RF field is controlled by bits 1:0 of TxControlReg.
   Value 3 (0b11) turns the antenna on. Without this check, the chip
   may appear to init but remain deaf to cards (common after supply
   glitches or loose SPI wires). */
    uint8_t v = rd(TxControlReg);
    if ((v & 3) != 3) {
        wr(TxControlReg, v | 3);
        /* Verify antenna enable took effect; if not, give up so the
           main loop can keep trying to re-initialize instead of
           silently operating in a broken state. */
        if ((rd(TxControlReg) & 3) != 3) {
            uint8_t ver = rd(VersionReg);
            return ver != 0x00 && ver != 0xFF;
        }
    }
    uint8_t ver = rd(VersionReg);
    return ver != 0x00 && ver != 0xFF;
}

bool rc522_wupa(void)
{
    clrmask(CollReg, 0x80);
    uint8_t cmd = 0x52, rx[2], n = 2;
    return xcvr(&cmd, 1, rx, &n, 7) == 0 && n == 2;
}

bool rc522_read_uid4(uint8_t uid[4])
{
    uint8_t ac[2] = { 0x93, 0x20 }, r[5], n = 5;
    if (xcvr(ac, 2, r, &n, 0) != 0 || n != 5) return false;
    if ((uint8_t)(r[0] ^ r[1] ^ r[2] ^ r[3]) != r[4]) return false;
    if (r[0] == 0x88) return false;                       /* cascade tag: 7-byte UID, unsupported */
    uint8_t sel[9] = { 0x93, 0x70, r[0], r[1], r[2], r[3], r[4], 0, 0 };
    if (!crc_a(sel, 7, &sel[7])) return false;
    uint8_t sak[3], m = 3;
    if (xcvr(sel, 9, sak, &m, 0) != 0 || m != 3) return false;
    if (sak[0] & 0x04) return false;                      /* UID not complete */
    memcpy(uid, r, 4);
    return true;
}

void rc522_halt(void)
{
    uint8_t h[4] = { 0x50, 0x00, 0, 0 }, rx[2], n = 2;
    if (crc_a(h, 2, &h[2])) (void)xcvr(h, 4, rx, &n, 0);  /* card does not answer; a timeout is the success case */
}
