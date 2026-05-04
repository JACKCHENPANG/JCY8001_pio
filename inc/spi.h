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
 */
void spi1_init(void);

/** SPI1 full-duplex single byte transfer */
uint8_t spi1_transfer(uint8_t tx_data);

/** SPI1 block transfer */
void spi1_transfer_buf(const uint8_t *pTxBuf, uint8_t *pRxBuf, uint16_t len);

/* ======================================================================== */
/* DNB1101 Daisy-Chain 协议 (新版) */
/* ======================================================================== */

#define DNB11XX_MAX_CHAIN  9

/** DNB1101 枚举 — 广播枚举所有 9 个 IC 槽位
 *  @param ic_ids_out  输出：发现的 IC ID 数组
 *  @param max_ics     输出数组最大容量
 *  @return 发现的 IC 数量
 */
uint8_t dnb1101_chain_enumerate(uint8_t *ic_ids_out, uint8_t max_ics);
uint8_t dnb1101_chain_init(void);  /* Init + SetMode — 枚举后必须调用 */

/** DNB1101 GetData — 向指定 IC 发送 GetData 命令
 *  @param ic_id       目标 IC ID
 *  @param data_type   GetData 类型 (0=MainVolt, 2=MainCellTemp, 6=VZM, 7=Zreal, 8=Zimag, 15=ProductVer)
 *  @param resp_data   输出：4字节响应
 *  @return 0=成功
 */
uint8_t dnb1101_chain_get_data(uint8_t ic_id, uint8_t data_type, uint8_t *resp_data);

/* ======================================================================== */
/* DNB1101 旧 ASCII 协议 (仅用于参考，DNB1101 不响应 ASCII 命令) */
/* ======================================================================== */

#define DNB_CMD_READ    0x52
#define DNB_CMD_WRITE   0x57
#define DNB_CMD_MEASURE 0x4D
#define DNB_CMD_STATUS  0x53

#define DNB_REG_VERSION    0x00
#define DNB_REG_STATUS     0x01
#define DNB_REG_VOLTAGE    0x02
#define DNB_REG_CURRENT    0x03
#define DNB_REG_IMPEDANCE  0x04
#define DNB_REG_TEMP       0x05
#define DNB_REG_MEASURE    0x10

uint8_t dnb1101_read_reg(uint8_t reg_addr, uint8_t *data, uint8_t len);
uint8_t dnb1101_write_reg(uint8_t reg_addr, const uint8_t *data, uint8_t len);
uint8_t dnb1101_get_version(uint8_t *version);
uint8_t dnb1101_get_status(uint8_t *status);
uint8_t dnb1101_get_voltage(uint32_t *voltage);
uint8_t dnb1101_get_impedance(int32_t *re, int32_t *im);
uint8_t dnb1101_get_temperature(int16_t *temp);
uint8_t dnb1101_start_measure(uint16_t freq_hz, uint8_t avg_count);

void spi1_dma_transfer(uint8_t *pTxBuf, uint8_t *pRxBuf, uint16_t len);
void spi1_dma_wait(void);

#endif /* SPI1_H */
