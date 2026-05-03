/**
 * spi.h - SPI1 driver for DNB1101 communication
 *
 * JCY8001 PlatformIO Project
 */

#ifndef SPI1_H
#define SPI1_H

#include <stdint.h>

/**
 * Initialize SPI1 for DNB1101 communication
 * PA5=SCK, PA6=MISO, PA7=MOSI
 * Clock: PCLK2 / 8 = ~1MHz (DNB1101 max 1MHz)
 * Note: No GPIO CS control - DNB1101 CS tied to capacitor only
 */
void spi1_init(void);

/**
 * SPI1 full-duplex transfer
 * @param tx_data data to transmit
 * @return received data
 */
uint8_t spi1_transfer(uint8_t tx_data);

/**
 * SPI1 block transfer
 * @param pTxBuf transmit buffer (can be NULL for read-only)
 * @param pRxBuf receive buffer (can be NULL for write-only)
 * @param len transfer length in bytes
 */
void spi1_transfer_buf(const uint8_t *pTxBuf, uint8_t *pRxBuf, uint16_t len);

/**
 * SPI1 DMA transfer (stub - not yet implemented)
 */
void spi1_dma_transfer(uint8_t *pTxBuf, uint8_t *pRxBuf, uint16_t len);

/**
 * Wait for DMA transfer to complete (stub)
 */
void spi1_dma_wait(void);

/* ======================================================================== */
/* DNB1101 芯片寄存器定义 */
/* ======================================================================== */

// DNB1101 命令字
#define DNB_CMD_READ    0x52  /* 'R' - 读寄存器 */
#define DNB_CMD_WRITE   0x57  /* 'W' - 写寄存器 */
#define DNB_CMD_MEASURE 0x4D  /* 'M' - 启动测量 */
#define DNB_CMD_STATUS  0x53  /* 'S' - 状态查询 */

// DNB1101 寄存器地址
#define DNB_REG_VERSION    0x00   /* 版本号 */
#define DNB_REG_STATUS     0x01   /* 状态寄存器 */
#define DNB_REG_VOLTAGE    0x02   /* 电压 (4字节，大端序，单位0.1mV) */
#define DNB_REG_CURRENT    0x03   /* 电流 (4字节) */
#define DNB_REG_IMPEDANCE  0x04   /* 阻抗 (8字节: RE 4B + IM 4B，大端序，Q16.16格式) */
#define DNB_REG_TEMP       0x05   /* 温度 */
#define DNB_REG_MEASURE    0x10   /* 测量控制寄存器 */

// DNB1101 操作函数
uint8_t dnb1101_read_reg(uint8_t reg_addr, uint8_t *data, uint8_t len);
uint8_t dnb1101_write_reg(uint8_t reg_addr, const uint8_t *data, uint8_t len);
uint8_t dnb1101_get_version(uint8_t *version);
uint8_t dnb1101_get_status(uint8_t *status);
uint8_t dnb1101_get_voltage(uint32_t *voltage);
uint8_t dnb1101_get_impedance(int32_t *re, int32_t *im);
uint8_t dnb1101_get_temperature(int16_t *temp);
uint8_t dnb1101_start_measure(uint16_t freq_hz, uint8_t avg_count);

#endif /* SPI1_H */
