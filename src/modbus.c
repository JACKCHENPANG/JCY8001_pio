/*
 * modbus.c - Modbus RTU protocol stack
 *
 * JCY8001 PlatformIO Project
 *
 * 协议规范:
 *   - RTU 模式, 8N1
 *   - CRC16 Modbus (polynomial 0xA001)
 */

#include "../inc/modbus.h"
#include "../inc/crc16.h"
#include "../inc/usart.h"

// ===== 全局变量 =====
volatile uint8_t g_mb_rx_buf[MB_RX_BUF_SIZE];
volatile uint8_t g_mb_rx_len = 0;
volatile uint8_t g_mb_tx_buf[MB_TX_BUF_SIZE];
volatile uint8_t g_mb_tx_len = 0;
volatile uint8_t g_mb_rx_ready = 0;

// ===== 内部状态 =====
typedef enum {
    MB_RX_IDLE,
    MB_RX_FUNC,
    MB_RX_DATA,
    MB_RX_CRC1,
    MB_RX_CRC2,
    MB_RX_DONE
} mb_rx_state_t;

static mb_rx_state_t rx_state = MB_RX_IDLE;
static uint8_t rx_buf[MB_RX_BUF_SIZE];
static uint8_t rx_index = 0;
static uint16_t rx_crc = 0xFFFF;

// ===== 接收字节处理 (供USART中断调用) =====
void modbus_rx_byte(uint8_t byte) {
    if (rx_index >= MB_RX_BUF_SIZE) {
        rx_index = 0;
        rx_state = MB_RX_IDLE;
        return;
    }

    switch (rx_state) {
        case MB_RX_IDLE:
            if (byte == MODBUS_ADDR) {  /* 只响应本机地址 */
                rx_buf[rx_index++] = byte;
                rx_crc = 0xFFFF;
                /* 预计算第一个字节的CRC */
                for (uint8_t i = 0; i < 8; i++) {
                    if (rx_crc & 0x0001) {
                        rx_crc = (rx_crc >> 1) ^ 0xA001;
                    } else {
                        rx_crc >>= 1;
                    }
                }
                rx_crc ^= byte;
                rx_state = MB_RX_FUNC;
            }
            break;

        case MB_RX_FUNC:
            rx_buf[rx_index++] = byte;
            for (uint8_t i = 0; i < 8; i++) {
                if (rx_crc & 0x0001) {
                    rx_crc = (rx_crc >> 1) ^ 0xA001;
                } else {
                    rx_crc >>= 1;
                }
            }
            rx_crc ^= byte;
            rx_state = MB_RX_DATA;
            break;

        case MB_RX_DATA:
            rx_buf[rx_index++] = byte;
            for (uint8_t i = 0; i < 8; i++) {
                if (rx_crc & 0x0001) {
                    rx_crc = (rx_crc >> 1) ^ 0xA001;
                } else {
                    rx_crc >>= 1;
                }
            }
            rx_crc ^= byte;

            /* 根据功能码决定数据长度 */
            if (rx_index >= 8) {
                rx_state = MB_RX_CRC1;
            }
            break;

        case MB_RX_CRC1:
            rx_buf[rx_index++] = byte;
            rx_state = MB_RX_CRC2;
            break;

        case MB_RX_CRC2:
            rx_buf[rx_index++] = byte;
            rx_state = MB_RX_DONE;
            break;

        default:
            rx_state = MB_RX_IDLE;
            rx_index = 0;
            break;
    }
}

// ===== Modbus 初始化 =====
void modbus_init(void) {
    rx_state = MB_RX_IDLE;
    rx_index = 0;
    g_mb_rx_len = 0;
    g_mb_rx_ready = 0;
    g_mb_tx_len = 0;
}

// ===== 发送 Modbus 响应 =====
void modbus_send_response(const uint8_t *data, uint8_t len) {
    g_mb_tx_len = 0;

    /* 计算CRC */
    uint16_t crc = crc16_modbus(data, len);
    uint8_t crc_lo = crc & 0xFF;
    uint8_t crc_hi = (crc >> 8) & 0xFF;

    /* 发送数据 + CRC */
    for (uint8_t i = 0; i < len; i++) {
        g_mb_tx_buf[g_mb_tx_len++] = data[i];
    }
    g_mb_tx_buf[g_mb_tx_len++] = crc_lo;
    g_mb_tx_buf[g_mb_tx_len++] = crc_hi;

    /* 通过USART发送 */
    uint8_t txbuf[MB_TX_BUF_SIZE];
    for (uint8_t i = 0; i < g_mb_tx_len; i++) {
        txbuf[i] = g_mb_tx_buf[i];
    }
    usart2_send_buf(txbuf, g_mb_tx_len);
}

// ===== 发送错误响应 =====
void modbus_send_error(uint8_t func, uint8_t ex_code) {
    uint8_t resp[3];
    resp[0] = MODBUS_ADDR;
    resp[1] = func | 0x80;
    resp[2] = ex_code;
    modbus_send_response(resp, 3);
}

// ===== 处理 Modbus 请求 =====
static void handle_request(void) {
    uint8_t func = rx_buf[1];
    uint16_t addr, count, value;
    uint8_t resp[256];

    resp[0] = MODBUS_ADDR;
    resp[1] = func;

    switch (func) {
        /* === FC01: 读线圈 === */
        case FC_READ_COILS:
            addr = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
            count = ((uint16_t)rx_buf[4] << 8) | rx_buf[5];

            if (count == 0 || count > 2000) {
                modbus_send_error(func, EX_ILLEGAL_DATA_VALUE);
                return;
            }

            resp[2] = (count + 7) / 8;
            for (uint16_t i = 0; i < count; i++) {
                uint8_t coil_val = read_coil(addr + i);
                uint8_t byte_pos = i / 8;
                uint8_t bit_pos = i % 8;
                if (byte_pos + 3 >= (uint8_t)sizeof(resp)) {
                    modbus_send_error(func, EX_SLAVE_DEVICE_FAILURE);
                    return;
                }
                if (coil_val) {
                    resp[3 + byte_pos] |= (1 << bit_pos);
                }
            }
            modbus_send_response(resp, 3 + resp[2]);
            break;

        /* === FC02: 读离散输入 === */
        case FC_READ_DISCRETE_INPUTS:
            addr = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
            count = ((uint16_t)rx_buf[4] << 8) | rx_buf[5];

            if (count == 0 || count > 2000) {
                modbus_send_error(func, EX_ILLEGAL_DATA_VALUE);
                return;
            }

            resp[2] = (count + 7) / 8;
            for (uint16_t i = 0; i < count; i++) {
                uint8_t input_val = read_discrete_input(addr + i);
                uint8_t byte_pos = i / 8;
                uint8_t bit_pos = i % 8;
                if (byte_pos + 3 >= (uint8_t)sizeof(resp)) {
                    modbus_send_error(func, EX_SLAVE_DEVICE_FAILURE);
                    return;
                }
                if (input_val) {
                    resp[3 + byte_pos] |= (1 << bit_pos);
                }
            }
            modbus_send_response(resp, 3 + resp[2]);
            break;

        /* === FC03: 读保持寄存器 === */
        case FC_READ_HOLDING_REG:
            addr = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
            count = ((uint16_t)rx_buf[4] << 8) | rx_buf[5];

            if (count == 0 || count > 125) {
                modbus_send_error(func, EX_ILLEGAL_DATA_VALUE);
                return;
            }

            resp[2] = (uint8_t)(count * 2);
            for (uint16_t i = 0; i < count; i++) {
                uint16_t val = read_holding_reg(addr + i);
                if (i * 2 + 5 >= (uint8_t)sizeof(resp)) {
                    modbus_send_error(func, EX_SLAVE_DEVICE_FAILURE);
                    return;
                }
                resp[3 + i * 2] = (uint8_t)((val >> 8) & 0xFF);
                resp[4 + i * 2] = (uint8_t)(val & 0xFF);
            }
            modbus_send_response(resp, 3 + resp[2]);
            break;

        /* === FC04: 读输入寄存器 === */
        case FC_READ_INPUT_REG:
            addr = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
            count = ((uint16_t)rx_buf[4] << 8) | rx_buf[5];

            if (count == 0 || count > 125) {
                modbus_send_error(func, EX_ILLEGAL_DATA_VALUE);
                return;
            }

            resp[2] = (uint8_t)(count * 2);
            for (uint16_t i = 0; i < count; i++) {
                uint16_t val = read_input_reg(addr + i);
                if (i * 2 + 5 >= (uint8_t)sizeof(resp)) {
                    modbus_send_error(func, EX_SLAVE_DEVICE_FAILURE);
                    return;
                }
                resp[3 + i * 2] = (uint8_t)((val >> 8) & 0xFF);
                resp[4 + i * 2] = (uint8_t)(val & 0xFF);
            }
            modbus_send_response(resp, 3 + resp[2]);
            break;

        /* === FC05: 写单个线圈 === */
        case FC_WRITE_SINGLE_COIL:
            addr = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
            value = ((uint16_t)rx_buf[4] << 8) | rx_buf[5];

            if (value != 0x0000 && value != 0xFF00) {
                modbus_send_error(func, EX_ILLEGAL_DATA_VALUE);
                return;
            }

            write_coil(addr, value == 0xFF00 ? 1 : 0);
            modbus_send_response(rx_buf, 8);
            break;

        /* === FC06: 写单个寄存器 === */
        case FC_WRITE_SINGLE_REG:
            addr = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
            value = ((uint16_t)rx_buf[4] << 8) | rx_buf[5];

            write_holding_reg(addr, value);
            modbus_send_response(rx_buf, 8);
            break;

        default:
            modbus_send_error(func, EX_ILLEGAL_FUNCTION);
            break;
    }
}

// ===== 验证并处理接收帧 =====
void modbus_process(void) {
    if (rx_state != MB_RX_DONE) return;

    /* 检查CRC */
    uint16_t calc = crc16_modbus(rx_buf, rx_index - 2);
    uint16_t recv_crc = ((uint16_t)rx_buf[rx_index - 2]) | ((uint16_t)rx_buf[rx_index - 1] << 8);

    if (calc != recv_crc) {
        /* CRC错误，丢弃 */
        rx_state = MB_RX_IDLE;
        rx_index = 0;
        return;
    }

    /* 处理请求 */
    handle_request();

    /* 准备接收下一帧 */
    rx_state = MB_RX_IDLE;
    rx_index = 0;
}

// ===== 轮询处理 (兼容原名) =====
void modbus_poll(void) {
    /* 从USART接收缓冲区读取所有字节 */
    int c;
    while ((c = usart2_getc()) >= 0) {
        modbus_rx_byte((uint8_t)c);
    }
    /* 处理完整帧 */
    modbus_process();
}
