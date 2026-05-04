/**
 * JCY8001 Modbus RTU Firmware v2.0 (PlatformIO bare-metal)
 * 
 * USART2 (PA2/PA3) 连接 CP2102, 115200 8N1
 * 支持: FC01, FC03, FC04, FC06
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

static void init_registers(void) {
    jcy_ch_count   = 1;
    jcy_version    = 0x0002;
    jcy_temp       = 250;
    jcy_voltage    = 6000;
    jcy_status     = 0x0003;
    jcy_zm_freq    = 40;
    jcy_zm_avg     = 10;
    jcy_fw_version = 0x0200;
    jcy_git_rev    = 0x0001;
    jcy_build_date = 0x0504;
}

static uint16_t get_reg(uint16_t addr) {
    switch (addr) {
        case 0x3E00: return jcy_ch_count;
        case 0x3E01: return jcy_version;
        case 0x3E02: return jcy_fw_version;
        case 0x3E03: return jcy_git_rev;
        case 0x3E04: return jcy_build_date;
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

    // PA2=AF_PP(TX), PA3=Input(RX)
    GPIOA->CRL = (GPIOA->CRL & 0xFFFF0000) | 0x00004B04;

    // 115200 @ 8MHz PCLK1
    USART2->BRR = 0x0045;
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
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
    usart2_send(crc & 0xFF);
    usart2_send(crc >> 8);
}

static void process_modbus(uint8_t *rx, uint16_t rxlen) {
    if (rxlen < 8) return;
    if (rx[0] != 0x01) return;

    uint16_t crc = crc16(rx, rxlen - 2);
    uint16_t rx_crc = rx[rxlen - 2] | (rx[rxlen - 1] << 8);
    if (crc != rx_crc) return;

    switch (rx[1]) {
        case 0x01: {  // Read Coils
            uint16_t addr = (rx[2] << 8) | rx[3];
            uint16_t count = (rx[4] << 8) | rx[5];
            tx_buf[0] = 0x01; tx_buf[1] = 0x01; tx_buf[2] = (count + 7) / 8;
            uint8_t *p = &tx_buf[3]; *p = 0;
            for (uint16_t i = 0; i < count; i++) {
                if (i && (i % 8 == 0)) *(++p) = 0;
                if (addr + i == 0) *p |= 0x01;
            }
            modbus_reply(tx_buf, 3 + tx_buf[2]);
            break;
        }
        case 0x03: case 0x04: {  // Read Holding/Input Registers
            uint16_t addr = (rx[2] << 8) | rx[3];
            uint16_t count = (rx[4] << 8) | rx[5];
            tx_buf[0] = 0x01; tx_buf[1] = rx[1]; tx_buf[2] = count * 2;
            uint8_t *p = &tx_buf[3];
            for (uint16_t i = 0; i < count; i++) {
                uint16_t v = get_reg(addr + i);
                *p++ = v >> 8; *p++ = v & 0xFF;
            }
            modbus_reply(tx_buf, 3 + count * 2);
            break;
        }
        case 0x06: {  // Write Single Register
            uint16_t addr = (rx[2] << 8) | rx[3];
            uint16_t val  = (rx[4] << 8) | rx[5];
            set_reg(addr, val);
            modbus_reply(rx, 6);  // Echo
            break;
        }
    }
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    // === 时钟重配置 ===
    // CubeF1 SystemInit 已（错误地）使能 HSE+PLL
    // 必须切换到 HSI 8MHz 并关闭 HSE/PLL
    RCC->CFGR = 0x00000000;               // SW=HSI
    RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_PLLON);  // 关闭 HSE+PLL
    RCC->CR |= RCC_CR_HSION;              // 确保 HSI 开启
    SystemCoreClock = 8000000;

    // 初始化寄存器 (绕过 .data 段复制问题)
    init_registers();

    // 初始化 USART2
    usart2_init();

    // 发送启动消息
    usart2_send('S'); usart2_send('T'); usart2_send('A');
    usart2_send('R'); usart2_send('T'); usart2_send('\r'); usart2_send('\n');

    // 主循环: 轮询USART2接收Modbus帧
    uint8_t  frame_buf[256];
    uint16_t frame_idx = 0;
    uint32_t idle_ticks = 0;

    while (1) {
        if (USART2->SR & USART_SR_RXNE) {
            uint8_t c = USART2->DR;
            if (frame_idx < sizeof(frame_buf))
                frame_buf[frame_idx++] = c;
            idle_ticks = 0;
        } else {
            idle_ticks++;
            // ~1ms 空闲后处理帧 (8MHz CPU, ~1000 loop cycles ≈ 1ms)
            if (frame_idx >= 8 && idle_ticks > 5000) {
                process_modbus(frame_buf, frame_idx);
                frame_idx = 0;
            }
        }
    }
}
