/**
 * register.h - JCY8001 Modbus register definitions
 *
 * JCY8001 PlatformIO Project
 *
 * 寄存器地址来自实测验证
 */

#ifndef REGISTER_H
#define REGISTER_H

#include <stdint.h>

// ===== 寄存器地址定义 =====
// 输入寄存器 (FC04)
#define REG_CHANNEL_COUNT   0x3E00  /* 通道数量 */
#define REG_FW_VERSION     0x3E01  /* 固件版本 */
#define REG_TEMPERATURE    0x3300  /* 温度 (×0.1°C) */
#define REG_VOLTAGE        0x3340  /* 电压 (×0.0001V) */
#define REG_STATUS         0x3380  /* 状态 */
#define REG_Z_RE           0x3000  /* 阻抗实部 */
#define REG_Z_IM           0x3080  /* 阻抗虚部 */
#define REG_Z_REAL         0x3100  /* Zreal */
#define REG_Z_VMAG         0x3200  /* VZM 数据 */
#define REG_FREQ           0x3280  /* 频率 */

// 保持寄存器 (FC03/06)
#define REG_ZM_FREQ        0x4000  /* ZM 频率 (Hz) */
#define REG_ZM_AVG_COUNT   0x4040  /* ZM 平均次数 */
#define REG_ZM_CYCLE_COUNT 0x4080  /* ZM 循环次数 */
#define REG_BALANCE_VOLT   0x4100  /* 均衡电压 */
#define REG_BALANCE_TIME   0x4140  /* 均衡时间 */
#define REG_PWM_DUTY       0x4180  /* PWM 占空比 */
#define REG_ZM_GAIN        0x4280  /* ZM 增益 */

// 保持寄存器 (FC03/06) - 群发
#define REG_ZM_AVG_COUNT_H 0x4F01  /* ZM 平均次数 (群发) */
#define REG_SAMPLE_RES     0x4F03  /* 采样电阻选择 (群发) */
#define REG_ZM_FREQ_H      0x4F07  /* 设置频率 (群发, 32位) */

// 线圈 (FC01/05)
#define COIL_START_MEASURE 0x0000  /* 启动测量 */

// ===== 寄存器操作 =====
void register_init(void);
void register_update_dnb1101(void);

// ===== 寄存器读写回调 =====
uint16_t read_holding_reg(uint16_t addr);
uint16_t read_input_reg(uint16_t addr);
uint8_t  read_coil(uint16_t addr);
uint8_t  read_discrete_input(uint16_t addr);
void     write_coil(uint16_t addr, uint8_t value);
void     write_holding_reg(uint16_t addr, uint16_t value);

#endif /* REGISTER_H */
