/**
 * Modbus协议处理 - 完整版
 * 移植自原始Keil工程，支持全部常用功能码
 */

#include "Modbus.h"
#include "CRC.h"
#include "Driver_USART1.h"
#include <string.h>

// ========== 静态变量 ==========
static Modbus_ReadRegCallback_t read_callback = NULL;
static Modbus_WriteRegCallback_t write_callback = NULL;
static Modbus_PublicData_t public_data;
static uint8_t tx_buf[256];

// ========== 公共寄存器读取 ==========
uint16_t Modbus_ReadPublicReg(uint16_t addr)
{
    // 3x区 (只读)
    if (addr >= REG_RE_START && addr < REG_RE_START + 64) {
        return 0;  // 实部数据
    }
    if (addr >= REG_IM_START && addr < REG_IM_START + 64) {
        return 0;  // 虚部数据
    }
    if (addr >= REG_ZREAL_START && addr < REG_ZREAL_START + 16) {
        return 0;  // 阻抗实部
    }
    if (addr >= REG_ZIMAG_START && addr < REG_ZIMAG_START + 16) {
        return 0;  // 阻抗虚部
    }
    if (addr >= REG_ZVOLT_START && addr < REG_ZVOLT_START + 16) {
        return 0;  // 阻抗电压
    }
    if (addr >= REG_VZM_START && addr < REG_VZM_START + 32) {
        return 0;  // 电压幅值
    }
    if (addr >= REG_FREQ_START && addr < REG_FREQ_START + 32) {
        return 0;  // 频率
    }
    if (addr >= REG_TEMP_START && addr < REG_TEMP_START + 16) {
        return public_data.ic_amount > 0 ? 250 : 0;  // 温度 25.0°C
    }
    if (addr >= REG_VOLT_START && addr < REG_VOLT_START + 16) {
        return 3700;  // 电压 3.700V
    }
    if (addr >= REG_STATUS_START && addr < REG_STATUS_START + 16) {
        return 0x0001;  // 状态正常
    }
    
    // 3E00-3E01
    if (addr == REG_IC_AMOUNT) {
        return public_data.ic_amount;
    }
    if (addr == REG_VERSION) {
        return public_data.version;
    }
    
    // 公共区
    if (addr == REG_COM_ZM_SW) {
        return public_data.zm_switch;
    }
    if (addr == REG_COM_BAL_SW) {
        return public_data.bal_switch;
    }
    if (addr == REG_COM_BAL_MODE) {
        return public_data.bal_mode;
    }
    
    return 0;
}

// ========== 公共寄存器写入 ==========
void Modbus_WritePublicReg(uint16_t addr, uint16_t value)
{
    if (addr >= REG_ZM_FREQ_START && addr < REG_ZM_FREQ_START + 16) {
        uint16_t freq_int = (value << 16) >> 16;
        public_data.zm_freq = freq_int;
        return;
    }
    if (addr >= REG_ZM_AVG_START && addr < REG_ZM_AVG_START + 16) {
        public_data.zm_avg_quantity = value;
        return;
    }
    if (addr >= REG_ZM_CYCLE_START && addr < REG_ZM_CYCLE_START + 16) {
        public_data.zm_cycle = value;
        return;
    }
    if (addr >= REG_ZM_RANGE_START && addr < REG_ZM_RANGE_START + 16) {
        public_data.zm_range = value;
        return;
    }
    if (addr >= REG_BAL_VOLT_START && addr < REG_BAL_VOLT_START + 16) {
        public_data.bal_volt = value;
        return;
    }
    if (addr >= REG_BAL_TIME_START && addr < REG_BAL_TIME_START + 16) {
        public_data.bal_time = value;
        return;
    }
    if (addr >= REG_BAL_PWM_START && addr < REG_BAL_PWM_START + 16) {
        public_data.bal_pwm = value;
        return;
    }
    
    // 公共区
    if (addr == REG_COM_ZM_SW) {
        public_data.zm_switch = value;
        return;
    }
    if (addr == REG_COM_BAL_SW) {
        public_data.bal_switch = value;
        return;
    }
    if (addr == REG_COM_BAL_MODE) {
        public_data.bal_mode = value;
        return;
    }
    
    // 用户回调
    if (write_callback) {
        write_callback(addr, value);
    }
}

// ========== 公共数据管理 ==========
void Modbus_SetPublicData(Modbus_PublicData_t *data)
{
    if (data) {
        memcpy(&public_data, data, sizeof(Modbus_PublicData_t));
    }
}

void Modbus_GetPublicData(Modbus_PublicData_t *data)
{
    if (data) {
        memcpy(data, &public_data, sizeof(Modbus_PublicData_t));
    }
}

// ========== 初始化 ==========
void Modbus_Init(void)
{
    memset(&public_data, 0, sizeof(public_data));
    public_data.version = 0x0100;  // v1.00
    public_data.ic_amount = 1;
    public_data.zm_switch = 0;
    public_data.bal_switch = 0;
    public_data.bal_mode = 0;
    public_data.zm_freq = 1000;
    public_data.zm_avg_quantity = 4;
    public_data.zm_cycle = 1;
    public_data.zm_range = 0;
    public_data.bal_volt = 3700;
    public_data.bal_time = 60;
    public_data.bal_pwm = 0;
}

// ========== 回调设置 ==========
void Modbus_SetReadCallback(Modbus_ReadRegCallback_t callback)
{
    read_callback = callback;
}

void Modbus_SetWriteCallback(Modbus_WriteRegCallback_t callback)
{
    write_callback = callback;
}

// ========== 发送 ==========
void Modbus_SendResponse(uint8_t *data, uint16_t len)
{
    USART1_Send(data, len, true);
}

// ========== 异常响应 ==========
static void send_exception(uint8_t slave_addr, uint8_t func_code, uint8_t error_code)
{
    tx_buf[0] = slave_addr;
    tx_buf[1] = func_code | 0x80;
    tx_buf[2] = error_code;
    uint16_t crc = Modbus_CRC16(tx_buf, 3);
    tx_buf[3] = crc >> 8;
    tx_buf[4] = crc & 0xFF;
    Modbus_SendResponse(tx_buf, 5);
}

// ========== FC01: 读线圈 ==========
static void process_read_coils(uint8_t slave_addr, uint8_t *data, uint16_t len)
{
    if (len < 5) {
        send_exception(slave_addr, MODBUS_FC_READ_COILS, MODBUS_ERR_ILLEGAL_ADDR);
        return;
    }
    
    uint16_t start_addr = (data[0] << 8) | data[1];
    uint16_t quantity = (data[2] << 8) | data[3];
    
    if (quantity < 1 || quantity > 2000) {
        send_exception(slave_addr, MODBUS_FC_READ_COILS, MODBUS_ERR_ILLEGAL_VALUE);
        return;
    }
    
    uint8_t byte_count = (quantity + 7) / 8;
    tx_buf[0] = slave_addr;
    tx_buf[1] = MODBUS_FC_READ_COILS;
    tx_buf[2] = byte_count;
    
    // 读取线圈状态 (使用公共数据)
    for (uint16_t i = 0; i < byte_count; i++) {
        uint8_t byte_val = 0;
        for (uint8_t bit = 0; bit < 8; bit++) {
            uint16_t coil_addr = start_addr + i * 8 + bit;
            uint16_t reg_val = Modbus_ReadPublicReg(coil_addr);
            if (reg_val != 0) {
                byte_val |= (1 << bit);
            }
        }
        tx_buf[3 + i] = byte_val;
    }
    
    uint16_t crc = Modbus_CRC16(data, 3 + byte_count);
    tx_buf[3 + byte_count] = crc >> 8;
    tx_buf[3 + byte_count + 1] = crc & 0xFF;
    Modbus_SendResponse(tx_buf, 5 + byte_count);
}

// ========== FC02: 读离散输入 ==========
static void process_read_discrete(uint8_t slave_addr, uint8_t *data, uint16_t len)
{
    if (len < 5) {
        send_exception(slave_addr, MODBUS_FC_READ_DISCRETE, MODBUS_ERR_ILLEGAL_ADDR);
        return;
    }
    
    uint16_t start_addr = (data[0] << 8) | data[1];
    uint16_t quantity = (data[2] << 8) | data[3];
    
    if (quantity < 1 || quantity > 2000) {
        send_exception(slave_addr, MODBUS_FC_READ_DISCRETE, MODBUS_ERR_ILLEGAL_VALUE);
        return;
    }
    
    uint8_t byte_count = (quantity + 7) / 8;
    tx_buf[0] = slave_addr;
    tx_buf[1] = MODBUS_FC_READ_DISCRETE;
    tx_buf[2] = byte_count;
    
    for (uint16_t i = 0; i < byte_count; i++) {
        uint8_t byte_val = 0;
        for (uint8_t bit = 0; bit < 8; bit++) {
            uint16_t coil_addr = start_addr + i * 8 + bit;
            uint16_t reg_val = Modbus_ReadPublicReg(0x1000 + (coil_addr - 0x1000));
            if (reg_val != 0) {
                byte_val |= (1 << bit);
            }
        }
        tx_buf[3 + i] = byte_val;
    }
    
    uint16_t crc = Modbus_CRC16(data, 3 + byte_count);
    tx_buf[3 + byte_count] = crc >> 8;
    tx_buf[3 + byte_count + 1] = crc & 0xFF;
    Modbus_SendResponse(tx_buf, 5 + byte_count);
}

// ========== FC03: 读保持寄存器 ==========
static void process_read_holding(uint8_t slave_addr, uint8_t *data, uint16_t len)
{
    if (len < 5) {
        send_exception(slave_addr, MODBUS_FC_READ_HOLDING, MODBUS_ERR_ILLEGAL_ADDR);
        return;
    }
    
    uint16_t start_addr = (data[0] << 8) | data[1];
    uint16_t reg_count = (data[2] << 8) | data[3];
    
    if (reg_count < 1 || reg_count > 125) {
        send_exception(slave_addr, MODBUS_FC_READ_HOLDING, MODBUS_ERR_ILLEGAL_VALUE);
        return;
    }
    
    tx_buf[0] = slave_addr;
    tx_buf[1] = MODBUS_FC_READ_HOLDING;
    tx_buf[2] = reg_count * 2;
    
    for (uint16_t i = 0; i < reg_count; i++) {
        uint16_t value = Modbus_ReadPublicReg(start_addr + i);
        tx_buf[3 + i * 2] = value >> 8;
        tx_buf[3 + i * 2 + 1] = value & 0xFF;
    }
    
    uint16_t crc = Modbus_CRC16(data, 3 + reg_count * 2);
    tx_buf[3 + reg_count * 2] = crc >> 8;
    tx_buf[3 + reg_count * 2 + 1] = crc & 0xFF;
    Modbus_SendResponse(tx_buf, 5 + reg_count * 2);
}

// ========== FC04: 读输入寄存器 ==========
static void process_read_input(uint8_t slave_addr, uint8_t *data, uint16_t len)
{
    if (len < 5) {
        send_exception(slave_addr, MODBUS_FC_READ_INPUT, MODBUS_ERR_ILLEGAL_ADDR);
        return;
    }
    
    uint16_t start_addr = (data[0] << 8) | data[1];
    uint16_t reg_count = (data[2] << 8) | data[3];
    
    if (reg_count < 1 || reg_count > 125) {
        send_exception(slave_addr, MODBUS_FC_READ_INPUT, MODBUS_ERR_ILLEGAL_VALUE);
        return;
    }
    
    tx_buf[0] = slave_addr;
    tx_buf[1] = MODBUS_FC_READ_INPUT;
    tx_buf[2] = reg_count * 2;
    
    for (uint16_t i = 0; i < reg_count; i++) {
        uint16_t value = Modbus_ReadPublicReg(start_addr + i);
        tx_buf[3 + i * 2] = value >> 8;
        tx_buf[3 + i * 2 + 1] = value & 0xFF;
    }
    
    uint16_t crc = Modbus_CRC16(data, 3 + reg_count * 2);
    tx_buf[3 + reg_count * 2] = crc >> 8;
    tx_buf[3 + reg_count * 2 + 1] = crc & 0xFF;
    Modbus_SendResponse(tx_buf, 5 + reg_count * 2);
}

// ========== FC05: 写单个线圈 ==========
static void process_write_single_coil(uint8_t slave_addr, uint8_t *data, uint16_t len)
{
    if (len < 5) {
        send_exception(slave_addr, MODBUS_FC_WRITE_SINGLE_COIL, MODBUS_ERR_ILLEGAL_ADDR);
        return;
    }
    
    uint16_t addr = (data[0] << 8) | data[1];
    uint16_t value = (data[2] << 8) | data[3];
    
    // 验证值: 0xFF00=ON, 0x0000=OFF
    if (value != 0xFF00 && value != 0x0000) {
        send_exception(slave_addr, MODBUS_FC_WRITE_SINGLE_COIL, MODBUS_ERR_ILLEGAL_VALUE);
        return;
    }
    
    // 写入公共寄存器
    Modbus_WritePublicReg(addr, value);
    
    // 回显
    tx_buf[0] = slave_addr;
    tx_buf[1] = MODBUS_FC_WRITE_SINGLE_COIL;
    memcpy(&tx_buf[2], data, 4);
    uint16_t crc = Modbus_CRC16(data, 6);
    tx_buf[6] = crc >> 8;
    tx_buf[7] = crc & 0xFF;
    Modbus_SendResponse(tx_buf, 8);
}

// ========== FC06: 写单个寄存器 ==========
static void process_write_single_reg(uint8_t slave_addr, uint8_t *data, uint16_t len)
{
    if (len < 5) {
        send_exception(slave_addr, MODBUS_FC_WRITE_SINGLE_REG, MODBUS_ERR_ILLEGAL_ADDR);
        return;
    }
    
    uint16_t addr = (data[0] << 8) | data[1];
    uint16_t value = (data[2] << 8) | data[3];
    
    Modbus_WritePublicReg(addr, value);
    
    // 回显
    tx_buf[0] = slave_addr;
    tx_buf[1] = MODBUS_FC_WRITE_SINGLE_REG;
    memcpy(&tx_buf[2], data, 4);
    uint16_t crc = Modbus_CRC16(data, 6);
    tx_buf[6] = crc >> 8;
    tx_buf[7] = crc & 0xFF;
    Modbus_SendResponse(tx_buf, 8);
}

// ========== FC0F: 写多个线圈 ==========
static void process_write_multi_coils(uint8_t slave_addr, uint8_t *data, uint16_t len)
{
    if (len < 6) {
        send_exception(slave_addr, MODBUS_FC_WRITE_MULTI_COILS, MODBUS_ERR_ILLEGAL_ADDR);
        return;
    }
    
    uint16_t start_addr = (data[0] << 8) | data[1];
    uint16_t quantity = (data[2] << 8) | data[3];
    uint8_t byte_count = data[4];
    
    if (byte_count != (quantity + 7) / 8) {
        send_exception(slave_addr, MODBUS_FC_WRITE_MULTI_COILS, MODBUS_ERR_ILLEGAL_VALUE);
        return;
    }
    
    for (uint16_t i = 0; i < quantity; i++) {
        uint8_t byte_val = data[5 + i / 8];
        uint16_t coil_val = (byte_val & (1 << (i % 8))) ? 0xFF00 : 0x0000;
        Modbus_WritePublicReg(start_addr + i, coil_val);
    }
    
    // 响应
    tx_buf[0] = slave_addr;
    tx_buf[1] = MODBUS_FC_WRITE_MULTI_COILS;
    tx_buf[2] = data[0];
    tx_buf[3] = data[1];
    tx_buf[4] = data[2];
    tx_buf[5] = data[3];
    uint16_t crc = Modbus_CRC16(data, 6);
    tx_buf[6] = crc >> 8;
    tx_buf[7] = crc & 0xFF;
    Modbus_SendResponse(tx_buf, 8);
}

// ========== FC10: 写多个寄存器 ==========
static void process_write_multi_regs(uint8_t slave_addr, uint8_t *data, uint16_t len)
{
    if (len < 6) {
        send_exception(slave_addr, MODBUS_FC_WRITE_MULTI_REGS, MODBUS_ERR_ILLEGAL_ADDR);
        return;
    }
    
    uint16_t start_addr = (data[0] << 8) | data[1];
    uint16_t reg_count = (data[2] << 8) | data[3];
    uint8_t byte_count = data[4];
    
    if (byte_count != reg_count * 2) {
        send_exception(slave_addr, MODBUS_FC_WRITE_MULTI_REGS, MODBUS_ERR_ILLEGAL_VALUE);
        return;
    }
    
    if (reg_count < 1 || reg_count > 125) {
        send_exception(slave_addr, MODBUS_FC_WRITE_MULTI_REGS, MODBUS_ERR_ILLEGAL_VALUE);
        return;
    }
    
    for (uint16_t i = 0; i < reg_count; i++) {
        uint16_t value = (data[5 + i * 2] << 8) | data[5 + i * 2 + 1];
        Modbus_WritePublicReg(start_addr + i, value);
    }
    
    // 响应
    tx_buf[0] = slave_addr;
    tx_buf[1] = MODBUS_FC_WRITE_MULTI_REGS;
    tx_buf[2] = data[0];
    tx_buf[3] = data[1];
    tx_buf[4] = data[2];
    tx_buf[5] = data[3];
    uint16_t crc = Modbus_CRC16(data, 6);
    tx_buf[6] = crc >> 8;
    tx_buf[7] = crc & 0xFF;
    Modbus_SendResponse(tx_buf, 8);
}

// ========== 主处理函数 ==========
void Modbus_Process(uint8_t *data, uint16_t len)
{
    if (len < 4) return;
    
    // CRC验证 (数据部分，不含CRC)
    uint16_t recv_crc = (data[len - 2] << 8) | data[len - 1];
    uint16_t calc_crc = Modbus_CRC16(data, len - 2);
    if (recv_crc != calc_crc) {
        return;  // CRC错误
    }
    
    uint8_t slave_addr = data[0];
    uint8_t func_code = data[1];
    
    // 地址过滤 (0=broadcast, MODBUS_SLAVE_ADDR=本机)
    if (slave_addr != MODBUS_SLAVE_ADDR && slave_addr != 0) {
        return;
    }
    
    // 执行命令 (广播不响应)
    if (slave_addr == 0) return;
    
    uint8_t *payload = &data[2];
    uint16_t payload_len = len - 4;  // 去掉 addr+func+crc
    
    switch (func_code) {
        case MODBUS_FC_READ_COILS:
            process_read_coils(slave_addr, payload, payload_len);
            break;
        case MODBUS_FC_READ_DISCRETE:
            process_read_discrete(slave_addr, payload, payload_len);
            break;
        case MODBUS_FC_READ_HOLDING:
            process_read_holding(slave_addr, payload, payload_len);
            break;
        case MODBUS_FC_READ_INPUT:
            process_read_input(slave_addr, payload, payload_len);
            break;
        case MODBUS_FC_WRITE_SINGLE_COIL:
            process_write_single_coil(slave_addr, payload, payload_len);
            break;
        case MODBUS_FC_WRITE_SINGLE_REG:
            process_write_single_reg(slave_addr, payload, payload_len);
            break;
        case MODBUS_FC_WRITE_MULTI_COILS:
            process_write_multi_coils(slave_addr, payload, payload_len);
            break;
        case MODBUS_FC_WRITE_MULTI_REGS:
            process_write_multi_regs(slave_addr, payload, payload_len);
            break;
        default:
            send_exception(slave_addr, func_code, MODBUS_ERR_ILLEGAL_FUNCTION);
            break;
    }
}
