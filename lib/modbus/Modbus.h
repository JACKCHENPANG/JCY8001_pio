/**
 * Modbus协议头文件 - 完整版
 * 移植自原始Keil工程
 */

#ifndef __MODBUS_H__
#define __MODBUS_H__

#include <stdint.h>
#include <stdbool.h>

// ========== 基本定义 ==========
#define MODBUS_SLAVE_ADDR         0x01

// 功能码
#define MODBUS_FC_READ_COILS         0x01
#define MODBUS_FC_READ_DISCRETE       0x02
#define MODBUS_FC_READ_HOLDING        0x03
#define MODBUS_FC_READ_INPUT          0x04
#define MODBUS_FC_WRITE_SINGLE_COIL   0x05
#define MODBUS_FC_WRITE_SINGLE_REG    0x06
#define MODBUS_FC_WRITE_MULTI_COILS   0x0F
#define MODBUS_FC_WRITE_MULTI_REGS    0x10

// 异常码
#define MODBUS_ERR_ILLEGAL_FUNCTION   0x01
#define MODBUS_ERR_ILLEGAL_ADDR       0x02
#define MODBUS_ERR_ILLEGAL_VALUE      0x03
#define MODBUS_ERR_SERVER_FAILURE     0x04

// ========== 寄存器地址定义 ==========
// 0x0000-0x003F: 阻抗测量开关
// 0x0040-0x007F: 平衡开关
// 0x0080-0x00BF: 平衡模式选择

// 0x0F00: 阻抗测量总开关
// 0x0F01: 平衡总开关
// 0x0F02: 平衡模式总控制

// 3x区 (只读) - 测量数据
#define REG_RE_START         0x3000  // 实部 (64通道×4寄存器=256字节)
#define REG_IM_START         0x3080  // 虚部
#define REG_ZREAL_START      0x3100  // 阻抗实部 (16通道×1)
#define REG_ZIMAG_START      0x3140  // 阻抗虚部
#define REG_ZVOLT_START      0x3180  // 阻抗电压
#define REG_VZM_START        0x3200  // 电压幅值 (32位)
#define REG_FREQ_START       0x3280  // 频率 (32位)
#define REG_TEMP_START       0x3300  // 温度 (16通道)
#define REG_VOLT_START       0x3340  // 电压 (16通道)
#define REG_STATUS_START     0x3380  // 状态

// 0x3E00: IC数量
#define REG_IC_AMOUNT        0x3E00
// 0x3E01: 版本号
#define REG_VERSION          0x3E01

// 4x区 (读写) - 参数设置
#define REG_ZM_FREQ_START     0x4000  // 阻抗测量频率
#define REG_ZM_AVG_START     0x4040  // 阻抗平均数量
#define REG_ZM_CYCLE_START   0x4080  // 阻抗循环次数
#define REG_ZM_RANGE_START   0x40C0  // 阻抗量程选择
#define REG_BAL_VOLT_START   0x4100  // 平衡电压
#define REG_BAL_TIME_START   0x4140  // 平衡时间
#define REG_BAL_PWM_START    0x4180  // PWM占空比

// 串口通信公共区
#define REG_COM_ZM_SW        0x0F00  // 阻抗测量开关
#define REG_COM_BAL_SW       0x0F01  // 平衡开关
#define REG_COM_BAL_MODE     0x0F02  // 平衡模式

// ========== 命令类型 ==========
typedef enum {
    MainCmd_Type_GetZMSwitch,
    MainCmd_Type_StartZM,
    MainCmd_Type_GetBalSwitch,
    MainCmd_Type_StartBal,
    MainCmd_Type_GetBalMode,
    MainCmd_Type_SetBalMode,
    MainCmd_Type_GetDataUp,
    MainCmd_Type_GetRE,
    MainCmd_Type_GetIM,
    MainCmd_Type_GetReal,
    MainCmd_Type_GetImag,
    MainCmd_Type_GetZVolt,
    MainCmd_Type_GetVZM,
    MainCmd_Type_GetSetZMFreq,
    MainCmd_Type_GetTemp,
    MainCmd_Type_GetVolt,
    MainCmd_Type_GetStatus,
    MainCmd_Type_GetICAmount,
    MainCmd_Type_GetVer,
    MainCmd_Type_GetZMAvgQuantity,
    MainCmd_Type_SetZMFreq,
    MainCmd_Type_SetZMAvgQuantity,
    MainCmd_Type_GetVolt2,
    MainCmd_Type_GetRange,
    MainCmd_Type_Overrun,
} MainCmd_Type_t;

// ========== 回调函数类型 ==========
typedef uint16_t (*Modbus_ReadRegCallback_t)(uint16_t addr);
typedef void (*Modbus_WriteRegCallback_t)(uint16_t addr, uint16_t value);
typedef MainCmd_Type_t (*Modbus_GetCmdCallback_t)(uint16_t addr, bool is_write);
typedef void (*Modbus_SetCmdCallback_t)(MainCmd_Type_t cmd, uint16_t value);

// ========== 公共区参数 (固件内部维护) ==========
typedef struct {
    uint8_t ic_amount;           // IC数量
    uint16_t version;            // 版本号
    uint16_t zm_switch;          // 阻抗测量开关
    uint16_t bal_switch;         // 平衡开关
    uint16_t bal_mode;           // 平衡模式
    float zm_freq;               // 阻抗测量频率
    uint16_t zm_avg_quantity;    // 平均数量
    uint16_t zm_cycle;           // 循环次数
    uint16_t zm_range;           // 量程选择
    float bal_volt;              // 平衡电压
    uint16_t bal_time;           // 平衡时间
    uint16_t bal_pwm;            // PWM占空比
    uint8_t data_update_flag;    // 数据更新标志
} Modbus_PublicData_t;

// ========== 函数接口 ==========
void Modbus_Init(void);
void Modbus_Process(uint8_t *data, uint16_t len);

// 回调设置
void Modbus_SetReadCallback(Modbus_ReadRegCallback_t callback);
void Modbus_SetWriteCallback(Modbus_WriteRegCallback_t callback);

// 公共数据接口
void Modbus_SetPublicData(Modbus_PublicData_t *data);
void Modbus_GetPublicData(Modbus_PublicData_t *data);
uint16_t Modbus_ReadPublicReg(uint16_t addr);
void Modbus_WritePublicReg(uint16_t addr, uint16_t value);

// 发送接口
void Modbus_SendResponse(uint8_t *data, uint16_t len);

#endif
