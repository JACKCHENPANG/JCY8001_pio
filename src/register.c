/*
 * register.c - JCY8001 register management + DNB1101 polling
 *
 * JCY8001 PlatformIO Project
 *
 * 整合 DNB1101 数据到 Modbus 寄存器
 */

#include "../inc/register.h"
#include "../inc/spi.h"

// ===== 寄存器数据 =====
static volatile uint16_t g_channel_count = 1;
static volatile uint16_t g_fw_version = 0x0100;    /* V1.0.0 */
static volatile uint16_t g_temperature = 250;         /* 25.0°C */
static volatile uint32_t g_voltage = 60000;         /* 6.0000V */
static volatile uint16_t g_status = 0x0003;        /* 无电池 */
static volatile int32_t  g_z_re = 0;               /* 阻抗实部 (Q16.16) */
static volatile int32_t  g_z_im = 0;               /* 阻抗虚部 (Q16.16) */
static volatile uint16_t g_zm_freq = 1000;          /* ZM 频率 Hz */
static volatile uint16_t g_zm_avg_count = 10;       /* 平均次数 */
static volatile uint16_t g_sample_res = 0;         /* 采样电阻 0=20R */

// ===== 寄存器初始化 =====
void register_init(void) {
    /* SPI1 初始化 (DNB1101) */
    spi1_init();

    /* 默认值 */
    g_channel_count = 1;
    g_fw_version = 0x0100;
    g_temperature = 250;
    g_voltage = 60000;
    g_status = 0x0003;
}

// ===== 定期更新 DNB1101 数据 =====
void register_update_dnb1101(void) {
    uint8_t status;
    uint32_t voltage;
    int32_t re, im;
    int16_t temp;

    /* 读取 DNB1101 状态 */
    if (dnb1101_get_status(&status) == 0) {
        g_status = status;
    }

    /* 读取电压 */
    if (dnb1101_get_voltage(&voltage) == 0) {
        g_voltage = voltage;
    }

    /* 读取阻抗 */
    if (dnb1101_get_impedance(&re, &im) == 0) {
        g_z_re = re;
        g_z_im = im;
    }

    /* 读取温度 */
    if (dnb1101_get_temperature(&temp) == 0) {
        g_temperature = (uint16_t)temp;
    }
}

// ===== 读输入寄存器 (FC04) =====
uint16_t read_input_reg(uint16_t addr) {
    switch (addr) {
        case 0x3E00:
            return g_channel_count;
        case 0x3E01:
            return g_fw_version;
        case 0x3300:
            return g_temperature;
        case 0x3340:
            return (uint16_t)(g_voltage & 0xFFFF);  /* 低16位 */
        case 0x3341:
            return (uint16_t)(g_voltage >> 16);     /* 高16位 */
        case 0x3380:
            return g_status;
        case 0x3000:
            return (uint16_t)(g_z_re & 0xFFFF);     /* RE 低16 */
        case 0x3001:
            return (uint16_t)(g_z_re >> 16);        /* RE 高16 */
        case 0x3080:
            return (uint16_t)(g_z_im & 0xFFFF);     /* IM 低16 */
        case 0x3081:
            return (uint16_t)(g_z_im >> 16);        /* IM 高16 */
        default:
            return 0;
    }
}

// ===== 读保持寄存器 (FC03) =====
uint16_t read_holding_reg(uint16_t addr) {
    switch (addr) {
        case 0x4000:
            return g_zm_freq;
        case 0x4040:
            return g_zm_avg_count;
        case 0x4F01:
            return g_zm_avg_count;
        case 0x4F03:
            return g_sample_res;
        default:
            return 0;
    }
}

// ===== 写保持寄存器 (FC06) =====
void write_holding_reg(uint16_t addr, uint16_t value) {
    switch (addr) {
        case 0x4000:
            g_zm_freq = value;
            break;
        case 0x4040:
            g_zm_avg_count = value;
            break;
        case 0x4F01:
            g_zm_avg_count = value;
            break;
        case 0x4F03:
            g_sample_res = value;
            break;
        default:
            break;
    }
}

// ===== 读线圈 (FC01) =====
uint8_t read_coil(uint16_t addr) {
    (void)addr;
    return 0;
}

// ===== 读离散输入 (FC02) =====
uint8_t read_discrete_input(uint16_t addr) {
    (void)addr;
    return 0;
}

// ===== 写线圈 (FC05) =====
void write_coil(uint16_t addr, uint8_t value) {
    if (addr == COIL_START_MEASURE && value == 1) {
        /* 启动 DNB1101 测量 */
        dnb1101_start_measure((uint16_t)g_zm_freq, (uint8_t)g_zm_avg_count);
    }
}
