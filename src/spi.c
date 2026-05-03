/*
 * spi.c - SPI1 driver for DNB1101 communication
 *
 * JCY8001 PlatformIO移植版
 *
 * 硬件连接 (无GPIO CS控制):
 * PA5=SCK, PA6=MISO, PA7=MOSI
 * DNB1101 CS pin 19 通过 C23(10uF) 接地，无GPIO控制
 */

#include "../inc/spi.h"
#include "../inc/stm32f1xx.h"

// SPI标志
#define SPI_SR_TXE    (1UL << 1)
#define SPI_SR_RXNE   (1UL << 0)
#define SPI_SR_BSY    (1UL << 7)

// ===== SPI1 初始化 =====
void spi1_init(void) {
    /* 使能时钟 */
    RCC->APB2ENR |= (1UL << 2) | (1UL << 12);  /* IOPAEN | SPI1EN */

    /* 配置GPIO:
     * PA5=SCK (AF PP), PA6=MISO (输入浮空), PA7=MOSI (AF PP)
     */
    uint32_t crl = GPIOA->CRL;
    crl &= ~(0xFFFUL << 20);  /* 清除 PA5-PA7 */
    crl |= (0xBUL << 20);     /* PA5: AF PP 50MHz */
    crl |= (0x4UL << 24);     /* PA6: Input floating */
    crl |= (0xBUL << 28);     /* PA7: AF PP 50MHz */
    GPIOA->CRL = crl;

    /* SPI1 配置:
     * - Master mode
     * - 8-bit data
     * - CPOL=0, CPHA=0 (Mode 0)
     * - 分频: PCLK2/8 = 8MHz/8 = 1MHz (DNB1101 max 1MHz)
     */
    SPI1->CR1 = 0x0000;
    SPI1->CR1 |= (1UL << 2);    /* MSTR: Master */
    SPI1->CR1 |= (3UL << 3);    /* BR[2:0] = 011: /8 => 1MHz OK */
    SPI1->CR1 |= (1UL << 6);    /* SPE: SPI enable */

    SPI1->CR2 = 0x0000;
}

// ===== SPI1 发送/接收 1 字节 =====
uint8_t spi1_transfer(uint8_t data) {
    /* 等待TX buffer空 */
    while ((SPI1->SR & SPI_SR_TXE) == 0);

    /* 发送数据 */
    SPI1->DR = data;

    /* 等待RX buffer非空 */
    while ((SPI1->SR & SPI_SR_RXNE) == 0);

    /* 读取接收数据 */
    return (uint8_t)SPI1->DR;
}

// ===== SPI1 缓冲区传输 =====
void spi1_transfer_buf(const uint8_t *tx_buf, uint8_t *rx_buf, uint16_t len) {
    while (len--) {
        uint8_t rx = spi1_transfer(*tx_buf++);
        if (rx_buf) {
            *rx_buf++ = rx;
        }
    }
}

// ===== DNB1101 读寄存器 =====
uint8_t dnb1101_read_reg(uint8_t reg_addr, uint8_t *data, uint8_t len) {
    uint8_t cmd_buf[3];

    /* 发送读命令: 'R' + 地址 + 长度 */
    cmd_buf[0] = DNB_CMD_READ;
    cmd_buf[1] = reg_addr;
    cmd_buf[2] = len;

    spi1_transfer_buf(cmd_buf, 0, 3);

    /* 读取数据 */
    spi1_transfer_buf(0, data, len);

    return 0;
}

// ===== DNB1101 写寄存器 =====
uint8_t dnb1101_write_reg(uint8_t reg_addr, const uint8_t *data, uint8_t len) {
    uint8_t cmd_buf[3];

    /* 发送写命令: 'W' + 地址 + 长度 */
    cmd_buf[0] = DNB_CMD_WRITE;
    cmd_buf[1] = reg_addr;
    cmd_buf[2] = len;

    spi1_transfer_buf(cmd_buf, 0, 3);
    spi1_transfer_buf(data, 0, len);

    return 0;
}

// ===== DNB1101 获取版本 =====
uint8_t dnb1101_get_version(uint8_t *version) {
    return dnb1101_read_reg(DNB_REG_VERSION, version, 1);
}

// ===== DNB1101 获取状态 =====
uint8_t dnb1101_get_status(uint8_t *status) {
    return dnb1101_read_reg(DNB_REG_STATUS, status, 1);
}

// ===== DNB1101 获取电压 =====
uint8_t dnb1101_get_voltage(uint32_t *voltage) {
    uint8_t data[4];
    uint8_t ret = dnb1101_read_reg(DNB_REG_VOLTAGE, data, 4);

    *voltage = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
               ((uint32_t)data[2] << 8) | data[3];

    return ret;
}

// ===== DNB1101 获取阻抗 =====
uint8_t dnb1101_get_impedance(int32_t *re, int32_t *im) {
    uint8_t data[8];
    uint8_t ret = dnb1101_read_reg(DNB_REG_IMPEDANCE, data, 8);

    /* 数据格式: RE(4字节) + IM(4字节)，大端序 */
    *re = ((int32_t)data[0] << 24) | ((int32_t)data[1] << 16) |
          ((int32_t)data[2] << 8) | data[3];
    *im = ((int32_t)data[4] << 24) | ((int32_t)data[5] << 16) |
          ((int32_t)data[6] << 8) | data[7];

    return ret;
}

// ===== DNB1101 获取温度 =====
uint8_t dnb1101_get_temperature(int16_t *temp) {
    uint8_t data[2];
    uint8_t ret = dnb1101_read_reg(DNB_REG_TEMP, data, 2);

    /* 数据格式: 大端序 int16，×0.1°C */
    *temp = ((int16_t)data[0] << 8) | data[1];

    return ret;
}

// ===== DNB1101 启动测量 =====
uint8_t dnb1101_start_measure(uint16_t freq_hz, uint8_t avg_count) {
    uint8_t cmd_buf[5];
    cmd_buf[0] = DNB_CMD_MEASURE;
    cmd_buf[1] = (uint8_t)(freq_hz >> 8);
    cmd_buf[2] = (uint8_t)(freq_hz & 0xFF);
    cmd_buf[3] = avg_count;
    cmd_buf[4] = (uint8_t)(cmd_buf[0] + cmd_buf[1] + cmd_buf[2] + cmd_buf[3]);

    return dnb1101_write_reg(DNB_REG_MEASURE, cmd_buf, 5);
}

// ===== DNB1101 DMA传输 (存根) =====
void spi1_dma_transfer(uint8_t *pTxBuf, uint8_t *pRxBuf, uint16_t len) {
    (void)pTxBuf;
    (void)pRxBuf;
    (void)len;
}

// ===== DNB1101 DMA等待 (存根) =====
void spi1_dma_wait(void) {
}
