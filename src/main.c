/**
 * JCY8001 Modbus RTU Firmware v2.3 (PlatformIO bare-metal)
 * 
 * USART2 (PA2/PA3) 连接 CP2102, 115200 8N1
 * SPI1 (PA5/PA6/PA7, PB2=NSS) DNB1101
 * 支持: FC01, FC03, FC04, FC06 + DNB1101 GetData
 */
#include <stdint.h>
#include "stm32f1xx.h"

/* ── JCY8001 Registers ──────────────────────────────────────────────────── */

volatile uint16_t jcy_ch_count;
volatile uint16_t jcy_version;
volatile uint16_t jcy_temp;
volatile uint16_t jcy_voltage;
volatile uint16_t jcy_status;
volatile uint16_t jcy_zm_freq;
volatile uint16_t jcy_zm_avg;
volatile uint16_t jcy_fw_version;
volatile uint16_t jcy_git_rev;
volatile uint16_t jcy_build_date;
volatile uint16_t jcy_dnb_debug;     // DNB枚举调试：版本字节
volatile uint16_t jcy_dnb_volt_raw;  // DNB GetData MainVolt 原始值
volatile uint16_t jcy_dnb_temp_raw;  // DNB GetData MainDieTemp 原始值
volatile uint16_t jcy_dnb_zmv_raw;   // DNB GetData VZM 原始值

static void init_registers(void) {
    jcy_ch_count   = 1;
    jcy_version    = 0x0002;
    jcy_temp       = 0;
    jcy_voltage    = 0;
    jcy_status     = 0x0003;
    jcy_zm_freq    = 40;
    jcy_zm_avg     = 10;
    jcy_fw_version = 0x0200;
    jcy_git_rev    = 0x0001;
    jcy_build_date = 0x0504;
    jcy_dnb_debug  = 0;
    jcy_dnb_volt_raw = 0;
    jcy_dnb_temp_raw = 0;
    jcy_dnb_zmv_raw  = 0;
}

static uint16_t get_reg(uint16_t addr) {
    switch (addr) {
        case 0x3E00: return jcy_ch_count;
        case 0x3E01: return jcy_version;
        case 0x3E02: return jcy_fw_version;
        case 0x3E03: return jcy_git_rev;
        case 0x3E04: return jcy_build_date;
        case 0x3E20: return jcy_dnb_debug;
        case 0x3E21: return jcy_dnb_volt_raw;
        case 0x3E22: return jcy_dnb_temp_raw;
        case 0x3E23: return jcy_dnb_zmv_raw;
        case 0x3300: return jcy_temp;
        case 0x3340: return jcy_voltage;
        case 0x3380: return jcy_status;
        case 0x4000: return jcy_zm_freq;
        case 0x4040: return jcy_zm_avg;
        default:     return 0x0000;
    }
}

static void set_reg(uint16_t addr, uint16_t val) {
    switch (addr) {
        case 0x3E00: jcy_ch_count = val; break;
        case 0x3E01: jcy_version = val; break;
        case 0x3E02: jcy_fw_version = val; break;
        case 0x3E03: jcy_git_rev = val; break;
        case 0x3E04: jcy_build_date = val; break;
        case 0x3300: jcy_temp = val; break;
        case 0x3340: jcy_voltage = val; break;
        case 0x3380: jcy_status = val; break;
        case 0x4000: jcy_zm_freq = val; break;
        case 0x4040: jcy_zm_avg = val; break;
    }
}

/* ── USART2 ─────────────────────────────────────────────────────────────── */

static void usart2_send(uint8_t c) {
    while (!(USART2->SR & USART_SR_TXE));
    USART2->DR = c;
}

static void usart2_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    GPIOA->CRL = (GPIOA->CRL & 0xFFFF0000) | 0x00004B04;
    USART2->BRR = 0x0045;
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

/* ── SPI1 (DNB1101) ─────────────────────────────────────────────────────── */

static uint8_t dnb_ic_count = 0;

static void spi1_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN | RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;
    GPIOA->CRL = (GPIOA->CRL & 0x0000FFFF) | 0xB4B00000;
    GPIOB->CRL = (GPIOB->CRL & 0xFFFFF0FF) | 0x00000300;
    GPIOB->BSRR = GPIO_BSRR_BS2;
    SPI1->CR1 = 0x0354;
}

static void spi1_nss_low(void)  { GPIOB->BRR  = GPIO_BRR_BR2; }
static void spi1_nss_high(void) { GPIOB->BSRR = GPIO_BSRR_BS2; }

static uint8_t spi1_transfer(uint8_t tx) {
    uint32_t timeout = 10000;
    while (!(SPI1->SR & SPI_SR_TXE) && --timeout);
    if (!timeout) return 0xFF;
    *(volatile uint8_t*)&SPI1->DR = tx;
    timeout = 10000;
    while (!(SPI1->SR & SPI_SR_RXNE) && --timeout);
    if (!timeout) return 0xFF;
    return *(volatile uint8_t*)&SPI1->DR;
}

static void spi1_full_duplex(uint8_t *tx, uint8_t *rx, uint32_t len) {
    spi1_nss_low();
    for (uint32_t i = 0; i < len; i++)
        rx[i] = spi1_transfer(tx[i]);
    spi1_nss_high();
}

/* ── CRC4 (DNB11xx protocol) ───────────────────────────────────────────── */

static const uint8_t crc4_tab[16] = {
    0x0, 0x3, 0x6, 0x5, 0xC, 0xF, 0xA, 0x9,
    0xB, 0x8, 0xD, 0xE, 0x7, 0x4, 0x1, 0x2,
};

static uint8_t crc4_nibble(const uint8_t *d, uint32_t n) {
    uint8_t c = 0xF;
    for (uint32_t i = 0; i < n; i++)
        c = crc4_tab[c ^ (d[i] & 0x0F)];
    return c & 0x0F;
}

/* ── DNB1101 Command Protocol ────────────────────────────────────────────── */

#define DNB_CMD_ENUMERATE  0x00
#define DNB_CMD_INIT       0x01
#define DNB_CMD_GETSTATUS  0x0D
#define DNB_CMD_GETDATA    0x0E

#define DNB_DATA_MAINVOLT     0x00
#define DNB_DATA_MAINDIETEMP  0x04
#define DNB_DATA_VZM          0x06

#define DNB_FRAME_LEN  9

/*
 * 标准 DNB11xx SPI 帧 (参考 DNB11xx_CreateSendBuf):
 *   [0x0F] [ID] [CMD|DataHi] [DataLo] [CRC4] [0xFF x4]
 * CRC4 计算覆盖前 3 字节 [0x0F, ID, CMD|DataHi]
 */
static uint8_t dnb_build_frame(uint8_t *txbuf, uint8_t id, uint8_t cmd, uint16_t data) {
    uint8_t c[4];
    c[3] = id;
    c[2] = (cmd << 4) | ((data >> 8) & 0x0F);
    c[1] = data & 0xFF;
    c[0] = 0;

    uint8_t crc_in[3] = {0x0F, c[3], c[2]};
    c[0] = crc4_nibble(crc_in, 3);

    txbuf[0] = 0x0F;
    txbuf[1] = c[3];
    txbuf[2] = c[2];
    txbuf[3] = c[1];
    txbuf[4] = c[0];
    txbuf[5] = 0xFF;
    txbuf[6] = 0xFF;
    txbuf[7] = 0xFF;
    txbuf[8] = 0xFF;

    return DNB_FRAME_LEN;
}

static uint8_t dnb_send_cmd(uint8_t id, uint8_t cmd, uint16_t data,
                             uint8_t *resp, uint8_t resp_len) {
    uint8_t tx[DNB_FRAME_LEN];
    uint8_t rx[DNB_FRAME_LEN];
    
    dnb_build_frame(tx, id, cmd, data);
    spi1_full_duplex(tx, rx, DNB_FRAME_LEN);

    for (uint8_t i = 0; i < resp_len && i < 4; i++)
        resp[i] = rx[5 + i];

    return 1;
}

/* ── DNB1101 Enumeration (simple probe) ─────────────────────────────────── */

static uint8_t dnb_enumerate(void) {
    uint8_t tx[4] = {0x52, 0x00, 0x01, 0xFF};
    uint8_t rx[4];
    spi1_full_duplex(tx, rx, 4);
    uint8_t ver = rx[3];
    jcy_dnb_debug = ver;
    return (ver != 0x00 && ver != 0xFF) ? 1 : 0;
}

/* ── DNB1101 Init ───────────────────────────────────────────────────────── */

static uint8_t dnb_init_ic(uint8_t id, uint8_t nr_of_ics) {
    uint8_t resp[4];
    uint16_t data = (0 << 8) | (nr_of_ics & 0xFF);  // AutoStb=0
    dnb_send_cmd(id, DNB_CMD_INIT, data, resp, 4);
    uint8_t ack = (resp[3] >> 4) & 0x0F;
    return (ack == 0x0A) ? 1 : 0;
}

/* ── DNB1101 GetData ────────────────────────────────────────────────────── */

static uint32_t dnb_get_data(uint8_t id, uint8_t data_type) {
    uint8_t resp[4] = {0};
    dnb_send_cmd(id, DNB_CMD_GETDATA, data_type, resp, 4);
    return ((uint32_t)resp[3] << 24) | ((uint32_t)resp[2] << 16) |
           ((uint32_t)resp[1] << 8)  | (uint32_t)resp[0];
}

/* ── CRC16 Modbus ───────────────────────────────────────────────────────── */

static uint16_t crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}

/* ── Modbus Frame Processing ────────────────────────────────────────────── */

static uint8_t tx_buf[256];

static void modbus_reply(const uint8_t *data, uint16_t len) {
    uint16_t crc = crc16(data, len);
    for (uint16_t i = 0; i < len; i++) usart2_send(data[i]);
    usart2_send(crc & 0xFF); usart2_send(crc >> 8);
}

static void process_modbus(uint8_t *rx, uint16_t rxlen) {
    if (rxlen < 8 || rx[0] != 0x01) return;
    uint16_t crc = crc16(rx, rxlen - 2);
    if (crc != (rx[rxlen - 2] | (rx[rxlen - 1] << 8))) return;

    switch (rx[1]) {
        case 0x01: {
            uint16_t addr = (rx[2] << 8) | rx[3], count = (rx[4] << 8) | rx[5];
            tx_buf[0] = 0x01; tx_buf[1] = 0x01; tx_buf[2] = (count + 7) / 8;
            uint8_t *p = &tx_buf[3]; *p = 0;
            for (uint16_t i = 0; i < count; i++) {
                if (i && (i % 8 == 0)) *(++p) = 0;
                if (addr + i == 0) *p |= 0x01;
            }
            modbus_reply(tx_buf, 3 + tx_buf[2]); break;
        }
        case 0x03: case 0x04: {
            uint16_t addr = (rx[2] << 8) | rx[3], count = (rx[4] << 8) | rx[5];
            tx_buf[0] = 0x01; tx_buf[1] = rx[1]; tx_buf[2] = count * 2;
            uint8_t *p = &tx_buf[3];
            for (uint16_t i = 0; i < count; i++) {
                uint16_t v = get_reg(addr + i);
                *p++ = v >> 8; *p++ = v & 0xFF;
            }
            modbus_reply(tx_buf, 3 + count * 2); break;
        }
        case 0x06: {
            uint16_t addr = (rx[2] << 8) | rx[3], val = (rx[4] << 8) | rx[5];
            set_reg(addr, val); modbus_reply(rx, 6); break;
        }
    }
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    RCC->CFGR = 0x00000000;
    RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_PLLON);
    RCC->CR |= RCC_CR_HSION;
    SystemCoreClock = 8000000;

    init_registers();
    usart2_init();
    spi1_init();

    // DNB1101 枚举 (需电池供电)
    dnb_ic_count = dnb_enumerate();
    if (dnb_ic_count > 0) {
        jcy_ch_count = dnb_ic_count;
        dnb_init_ic(1, dnb_ic_count);  // Init IC #1
        jcy_status = 0x0001;
    }

    uint8_t  frame_buf[256];
    uint16_t frame_idx = 0;
    uint32_t idle_ticks = 0;
    uint32_t measure_ticks = 0;
    uint8_t  measure_phase = 0;

#define MEASURE_INTERVAL  50000   // ~500ms @ 8MHz

    while (1) {
        if (USART2->SR & USART_SR_RXNE) {
            uint8_t c = USART2->DR;
            if (frame_idx < sizeof(frame_buf)) frame_buf[frame_idx++] = c;
            idle_ticks = 0;
        } else {
            idle_ticks++;
            measure_ticks++;

            if (frame_idx >= 8 && idle_ticks > 5000) {
                process_modbus(frame_buf, frame_idx);
                frame_idx = 0;
            }

            // DNB1101 定期测量 (需电池供电)
            if (dnb_ic_count > 0 && measure_ticks >= MEASURE_INTERVAL) {
                measure_ticks = 0;
                switch (measure_phase) {
                case 0: {
                    uint32_t raw = dnb_get_data(1, DNB_DATA_MAINVOLT);
                    jcy_dnb_volt_raw = raw & 0xFFFF;
                    uint32_t adc = (raw >> 8) & 0x3FFF;
                    jcy_voltage = (uint16_t)((adc * 4800UL) / 16383UL + 1200UL);
                    break;
                }
                case 1: {
                    uint32_t raw = dnb_get_data(1, DNB_DATA_MAINDIETEMP);
                    jcy_dnb_temp_raw = raw & 0xFFFF;
                    jcy_temp = raw & 0xFFFF;
                    break;
                }
                case 2: {
                    uint32_t raw = dnb_get_data(1, DNB_DATA_VZM);
                    jcy_dnb_zmv_raw = raw & 0xFFFF;
                    break;
                }
                }
                measure_phase = (measure_phase + 1) % 3;
            }
        }
    }
}
