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

/* 全局调试变量 — 可通过 J-Link 读取 */
volatile uint8_t g_dnb_last_rx[9 * 4];  /* DNB11XX_MAX_CHAIN * 4 */
volatile uint8_t g_dnb_ic_count = 0;
volatile uint8_t g_dnb_ic_id = 0;

// SPI标志
#define SPI_SR_TXE    (1UL << 1)
#define SPI_SR_RXNE   (1UL << 0)
#define SPI_SR_BSY    (1UL << 7)

// ===== SPI1 初始化 =====
void spi1_init(void) {
    /* 使能时钟 */
    RCC->APB2ENR |= (1UL << 0) | (1UL << 2) | (1UL << 12);  /* AFIOEN | IOPAEN | SPI1EN */

    /* 配置GPIOA — 用裸指针绕过 CMSIS，防止编译器/链接器问题
     * PA0: 4(浮空), PA1: 4(浮空), PA2: B(AF PP USART2_TX), PA3: 4(浮空 USART2_RX)
     * PA4: 4(浮空), PA5: B(AF PP SPI1_SCK), PA6: 4(浮空 SPI1_MISO), PA7: B(AF PP SPI1_MOSI)
     */
    *(volatile uint32_t *)0x40010800 = 0xB4B44B44;

    /* SPI1 配置:
     * - Master mode, 8-bit data, CPOL=0/CPHA=0 (Mode 0)
     * - SSM+SSI: 软件从模式管理（PA4浮空会触发Mode Fault清除MSTR！）
     * - 分频: PCLK2/8 = 8MHz/8 = 1MHz (DNB1101 max 1MHz)
     * - 一次写入防止Mode Fault在两步之间触发
     *   0x0354 = SSM(bit9) + SSI(bit8) + SPE(bit6) + BR=010(bit4) + MSTR(bit2)
     */
    SPI1->CR1 = 0x0354;

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

// ===== DNB1101 读寄存器 (旧 ASCII 协议 — 仅用于参考，DNB1101 不响应) =====
uint8_t dnb1101_read_reg(uint8_t reg_addr, uint8_t *data, uint8_t len) {
    uint8_t cmd_buf[3];
    cmd_buf[0] = DNB_CMD_READ;
    cmd_buf[1] = reg_addr;
    cmd_buf[2] = len;
    spi1_transfer_buf(cmd_buf, 0, 3);
    spi1_transfer_buf(0, data, len);
    return 0;
}

uint8_t dnb1101_write_reg(uint8_t reg_addr, const uint8_t *data, uint8_t len) {
    uint8_t cmd_buf[3];
    cmd_buf[0] = DNB_CMD_WRITE;
    cmd_buf[1] = reg_addr;
    cmd_buf[2] = len;
    spi1_transfer_buf(cmd_buf, 0, 3);
    spi1_transfer_buf(data, 0, len);
    return 0;
}

uint8_t dnb1101_get_version(uint8_t *version) {
    return dnb1101_read_reg(DNB_REG_VERSION, version, 1);
}

uint8_t dnb1101_get_status(uint8_t *status) {
    return dnb1101_read_reg(DNB_REG_STATUS, status, 1);
}

uint8_t dnb1101_get_voltage(uint32_t *voltage) {
    uint8_t data[4];
    uint8_t ret = dnb1101_read_reg(DNB_REG_VOLTAGE, data, 4);
    *voltage = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
               ((uint32_t)data[2] << 8) | data[3];
    return ret;
}

uint8_t dnb1101_get_impedance(int32_t *re, int32_t *im) {
    uint8_t data[8];
    uint8_t ret = dnb1101_read_reg(DNB_REG_IMPEDANCE, data, 8);
    *re = ((int32_t)data[0] << 24) | ((int32_t)data[1] << 16) |
          ((int32_t)data[2] << 8) | data[3];
    *im = ((int32_t)data[4] << 24) | ((int32_t)data[5] << 16) |
          ((int32_t)data[6] << 8) | data[7];
    return ret;
}

uint8_t dnb1101_get_temperature(int16_t *temp) {
    uint8_t data[2];
    uint8_t ret = dnb1101_read_reg(DNB_REG_TEMP, data, 2);
    *temp = ((int16_t)data[0] << 8) | data[1];
    return ret;
}

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

void spi1_dma_wait(void) {
}

/* ========================================================================
 * DNB1101 Daisy-Chain 协议 (新版)
 * ========================================================================
 *
 * 帧格式 (4字节/slot):
 *   Byte0: 0x0F (帧头)
 *   Byte1: ID (目标IC地址, 0xFF=广播)
 *   Byte2: CMD[3:0]<<4 | Data[15:12]
 *   Byte3: CRC4[3:0]<<4 | Data[11:8]<<0 ... 等等
 *
 * 实际 wire format (从 lib/dnb11xx/dnb11xx.c):
 *   buf[0] = 0x0F
 *   buf[1] = ID
 *   buf[2] = CMD<<4 | Data_high_nibble
 *   buf[3] = CRC4(buf[0..2])<<4 | Data_mid_nibble
 */

#define DNB11XX_MAX_CHAIN  9
#define DNB11XX_SLOT_SIZE  4

/* CRC4 — CRC-4-ITU polynomial x^4+x^3+1 (truncated 0x09)
 * 使用原始代码的 crc_array 表 */
static const uint8_t CRC4_ITU_TABLE[16] = {
    0x00, 0x09, 0x0B, 0x02, 0x0F, 0x06, 0x04, 0x0D,
    0x07, 0x0E, 0x0C, 0x05, 0x08, 0x01, 0x03, 0x0A,
};

/* CRC4 over 4 command bytes, nibble-by-nibble (LSB first per byte)
 * 返回 4-bit CRC 值 */
static uint8_t dnb1101_crc4_cmd(const uint8_t *cmd4) {
    uint8_t crc = 0;
    /* Process each byte's two nibbles: low nibble first, then high nibble */
    for (int i = 0; i < 4; i++) {
        crc = CRC4_ITU_TABLE[crc ^ (cmd4[i] & 0x0F)];
        crc = CRC4_ITU_TABLE[crc ^ (cmd4[i] >> 4)];
    }
    return crc;
}

/* 发送前导码: 430×0x00 + 4×0x01 = 430字节0 + 1字节0xF0
 * 手册 6.9.6.8: "至少60个0 + 4个1" 唤醒IC */
static void dnb1101_send_preamble(void) {
    for (int i = 0; i < 430; i++) {
        spi1_transfer(0x00);
    }
    spi1_transfer(0xF0);  /* 4个1 */
    /* 手册: IC在唤醒后等待8个零, 再等8个时钟开始转发 */
    for (int i = 0; i < 16; i++) {
        spi1_transfer(0x00);
    }
}

#define DNB11XX_FRAME_PER_IC  5   /* 0x0F + 4 命令字节 = 5 字节/IC (原始格式) */

/* 构建一个 IC 的命令帧 (5字节, 匹配原始 DNB11xx_CreateSendBuf)
 * 帧格式: 0x0F, ID, CMD|Data_high, Data_low, CRC|Data[3:0] */
static void chain_build_cmd(uint8_t *cmd5, uint8_t id, uint8_t cmd_type, uint16_t data_val) {
    /* 构建 4 字节命令 (CRC 先填 0) */
    uint8_t ucData[4];
    ucData[0] = (uint8_t)((data_val & 0x0F) << 4);  /* Data[3:0] in upper nibble, CRC=0 */
    ucData[1] = (uint8_t)((data_val >> 4) & 0xFF);   /* Data[11:4] */
    ucData[2] = (uint8_t)((cmd_type << 4) | ((data_val >> 12) & 0x0F)); /* CMD|Data[15:12] */
    ucData[3] = id;                                   /* ID */

    /* 计算 CRC4 over 4 字节 */
    uint8_t crc = dnb1101_crc4_cmd(ucData);

    /* 组装 5 字节帧 */
    cmd5[0] = 0x0F;
    cmd5[1] = id;
    cmd5[2] = ucData[2];  /* CMD|Data_high */
    cmd5[3] = ucData[1];  /* Data_low */
    cmd5[4] = (uint8_t)(((data_val & 0x0F) << 4) | crc);  /* Data[3:0]|CRC */
}

/*
 * DNB1101 完整初始化和枚举流程 (匹配原始 DNB11xxThread)
 *
 * Phase 1: GetStatus(CheckID, ID=1) + Enum if needed
 * Phase 2: Init(IC_Amount=1, NoAutoStb, EnSrvReq)
 * Phase 3: SetMode(Normal, ID=0xFF)
 *
 * 关键: HeadLen=429 用于首次通讯, HeadLen=8 用于后续命令
 *       Set 命令必须发两次
 */
uint8_t dnb1101_chain_enumerate(uint8_t *ic_ids_out, uint8_t max_ics) {
    uint8_t cmd5[5];
    int i;

    /* ═══ Phase 1: GetStatus(CheckID, ID=1) ═══ */
    chain_build_cmd(cmd5, 1, 0x0D, 0x00);  /* CMD=GetStatus, Data=CheckID(0x00) */

    /* HeadLen=429: 428×0x00 + 0x0F + cmd + 4×0xFF + 0xF0 */
    for (i = 0; i < 428; i++) spi1_transfer(0x00);
    spi1_transfer(0x0F);
    uint8_t rx[500];
    for (i = 0; i < 4; i++) rx[i] = spi1_transfer(cmd5[i+1]);  /* cmd5[1..4] = command bytes */
    for (i = 0; i < 4; i++) spi1_transfer(0xFF);
    spi1_transfer(0xF0);

    /* Check response: ID==0 means no IC, need Enum */
    if (rx[0] == 0 || rx[0] == 0xFF) {
        /* ── No IC at slot 1, send Enum command ── */
        /* Enum data: SetID=1, IgnoreBcast=0, SpltBus=0, WriteMTP=0 */
        chain_build_cmd(cmd5, 0, 0x00, 1);  /* CMD=Enumerate, Data=SetID(1) */

        /* HeadLen=8: 7×0x00 + 0x0F + cmd + (1*4)×0xFF + 0xF0 */
        for (i = 0; i < 7; i++) spi1_transfer(0x00);
        spi1_transfer(0x0F);
        for (i = 0; i < 4; i++) spi1_transfer(cmd5[i+1]);
        for (i = 0; i < 4; i++) spi1_transfer(0xFF);
        spi1_transfer(0xF0);

        /* 延迟 2ms */
        /* 第二次 Enum (SET命令必须发两次) */
        for (i = 0; i < 7; i++) spi1_transfer(0x00);
        spi1_transfer(0x0F);
        for (i = 0; i < 4; i++) spi1_transfer(cmd5[i+1]);
        for (i = 0; i < 4; i++) spi1_transfer(0xFF);
        spi1_transfer(0xF0);
    }

    g_dnb_ic_count = (rx[0] != 0 && rx[0] != 0xFF) ? 1 : 0;
    if (g_dnb_ic_count > 0 && max_ics > 0) {
        g_dnb_ic_id = rx[0];
        ic_ids_out[0] = rx[0];
    }
    return g_dnb_ic_count;
}

/*
 * DNB1101 Init + SetMode (必须在 Enum 之后调用)
 * 返回 0=成功
 */
uint8_t dnb1101_chain_init(void) {
    uint8_t cmd5[5];
    int i;

    if (g_dnb_ic_count == 0) return 1;

    /* ═══ Phase 2: Init(NrOfICs=1, NoAutoStb, EnSrvReq) ═══ */
    /* Init data: NrOfICs=1, AutoStb=0, ResetID=0, GenPOR=0, EnSrvReq=1, ReloadMTP=0, WriteMTP=0
     * = 1 | (1 << 12) = 0x1001 */
    chain_build_cmd(cmd5, g_dnb_ic_id, 0x01, 0x1001);  /* CMD=Init */

    /* HeadLen=8, 发两次 */
    for (int attempt = 0; attempt < 2; attempt++) {
        for (i = 0; i < 7; i++) spi1_transfer(0x00);
        spi1_transfer(0x0F);
        for (i = 0; i < 4; i++) spi1_transfer(cmd5[i+1]);
        for (i = 0; i < 4; i++) spi1_transfer(0xFF);
        spi1_transfer(0xF0);
    }

    /* ═══ Phase 3: SetMode(Normal, ID=0xFF广播) ═══ */
    chain_build_cmd(cmd5, 0xFF, 0x0B, 4);  /* CMD=SetMode, Data=Normal(4) */

    /* HeadLen=8, 发两次 */
    for (int attempt = 0; attempt < 2; attempt++) {
        for (i = 0; i < 7; i++) spi1_transfer(0x00);
        spi1_transfer(0x0F);
        for (i = 0; i < 4; i++) spi1_transfer(cmd5[i+1]);
        for (i = 0; i < g_dnb_ic_count * 4; i++) spi1_transfer(0xFF);
        spi1_transfer(0xF0);
    }

    return 0;
}

/*
 * DNB1101 GetData — 向指定 IC 发送 GetData 命令
 * ic_id: 目标 IC 的 ID (从枚举获得)
 * data_type: GetData 类型 (0=电压, 2=温度, 6=VZM, 7=Zreal, 8=Zimag, 15=版本...)
 * resp_data: 输出，响应的原始 4 字节
 * 返回: 0=成功, 非0=失败
 */
uint8_t dnb1101_chain_get_data(uint8_t ic_id, uint8_t data_type, uint8_t *resp_data) {
    uint8_t cmd5[5];
    chain_build_cmd(cmd5, ic_id, 0x0E, data_type);  /* CMD=GetData */

    uint8_t rx[500];
    int idx = 0;

    /* 430×0x00 前导码 */
    for (int i = 0; i < 430; i++) rx[idx++] = spi1_transfer(0x00);

    /* 命令帧 5 字节 */
    for (int i = 0; i < 5; i++) rx[idx++] = spi1_transfer(cmd5[i]);

    /* 0xFF 填充 */
    for (int i = 0; i < DNB11XX_MAX_CHAIN * 4; i++) rx[idx++] = spi1_transfer(0xFF);

    /* 帧尾 0xF0 */
    rx[idx++] = spi1_transfer(0xF0);

    /* 保存 */
    for (int i = 0; i < 36; i++) g_dnb_last_rx[i] = (i < idx) ? rx[i] : 0;

    /* 解析响应 */
    int cmd_end = 430 + 5;
    for (int slot = 0; slot < DNB11XX_MAX_CHAIN; slot++) {
        int base = cmd_end + slot * 4;
        if (base + 3 >= idx) break;
        if (rx[base + 1] == ic_id) {
            for (int i = 0; i < 4; i++) resp_data[i] = rx[base + i];
            return 0;
        }
    }
    return 1;
}
