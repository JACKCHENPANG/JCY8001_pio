/**
 * modbus.h - Modbus RTU protocol stack
 *
 * JCY8001 PlatformIO Project
 *
 * 支持:
 *   FC01 - 读线圈
 *   FC02 - 读离散输入
 *   FC03 - 读保持寄存器
 *   FC04 - 读输入寄存器
 *   FC05 - 写单个线圈
 *   FC06 - 写单个寄存器
 */

#ifndef MODBUS_H
#define MODBUS_H

#include <stdint.h>

// ===== Modbus 地址定义 =====
#define MODBUS_ADDR  0x01        /* 设备地址 */

// ===== 功能码 =====
#define FC_READ_COILS            0x01
#define FC_READ_DISCRETE_INPUTS  0x02
#define FC_READ_HOLDING_REG      0x03
#define FC_READ_INPUT_REG        0x04
#define FC_WRITE_SINGLE_COIL     0x05
#define FC_WRITE_SINGLE_REG      0x06

// ===== 错误码 =====
#define EX_ILLEGAL_FUNCTION      0x01
#define EX_ILLEGAL_DATA_ADDR    0x02
#define EX_ILLEGAL_DATA_VALUE   0x03
#define EX_SLAVE_DEVICE_FAILURE  0x04

// ===== 帧缓冲区 =====
#define MB_RX_BUF_SIZE  256
#define MB_TX_BUF_SIZE  256

// ===== 全局变量 =====
extern volatile uint8_t g_mb_rx_buf[MB_RX_BUF_SIZE];
extern volatile uint8_t g_mb_rx_len;
extern volatile uint8_t g_mb_tx_buf[MB_TX_BUF_SIZE];
extern volatile uint8_t g_mb_tx_len;
extern volatile uint8_t g_mb_rx_ready;

// ===== 函数接口 =====
void modbus_init(void);
void modbus_poll(void);         /* 轮询处理 (兼容原名) */
void modbus_process(void);      /* 处理接收帧 */
void modbus_rx_byte(uint8_t byte);
void modbus_send_response(const uint8_t *data, uint8_t len);
void modbus_send_error(uint8_t func, uint8_t ex_code);

// 寄存器读写回调 (由应用层实现)
uint16_t read_holding_reg(uint16_t addr);
uint16_t read_input_reg(uint16_t addr);
uint8_t  read_coil(uint16_t addr);
uint8_t  read_discrete_input(uint16_t addr);
void     write_coil(uint16_t addr, uint8_t value);
void     write_holding_reg(uint16_t addr, uint16_t value);

#endif /* MODBUS_H */
