/**
 * USART1驱动 - HAL库版本
 * 用于Modbus通信 (115200 8N1)
 * 移植自原始Keil工程
 */

#include "Driver_USART1.h"
#include <string.h>

// 接收缓冲区
#define RX_BUF_LEN  256
static uint8_t rx_buf[RX_BUF_LEN];
static uint16_t rx_count = 0;
static bool rx_busy = false;

static USART1_RxCallback_t rx_callback = NULL;

// 空闲检测定时器相关
static uint32_t last_rx_tick = 0;
#define IDLE_TIMEOUT_MS  5

// DMA发送完成标志
static volatile bool tx_complete = true;

/**
 * USART1初始化 (HAL版本)
 */
void USART1_Init(void)
{
    // UART已在main.c中通过MX_USART1_UART_Init()初始化
    // 这里启动接收中断
    HAL_UART_Receive_IT(&huart1, &rx_buf[0], 1);
}

/**
 * 设置接收回调函数
 */
void USART1_SetRxCallback(USART1_RxCallback_t callback)
{
    rx_callback = callback;
}

/**
 * 设置波特率
 */
void USART1_SetBaudRate(uint32_t baudrate)
{
    huart1.Init.BaudRate = baudrate;
    HAL_UART_Init(&huart1);
}

/**
 * 发送数据
 * @param p 数据指针
 * @param Length 数据长度
 * @param isWaiting 是否等待发送完成
 */
void USART1_Send(uint8_t *p, uint16_t Length, bool isWaiting)
{
    if (isWaiting) {
        HAL_UART_Transmit(&huart1, p, Length, 1000);
    } else {
        tx_complete = false;
        HAL_UART_Transmit_IT(&huart1, p, Length);
    }
}

/**
 * UART接收中断回调 (HAL库调用)
 * 每收到1字节触发，存入缓冲区
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        last_rx_tick = HAL_GetTick();
        rx_busy = true;
        
        // 当前字节已存入 rx_buf[rx_count]
        rx_count++;
        
        // 继续接收下一个字节
        if (rx_count < RX_BUF_LEN) {
            HAL_UART_Receive_IT(&huart1, &rx_buf[rx_count], 1);
        }
    }
}

/**
 * UART发送完成回调
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        tx_complete = true;
    }
}

/**
 * 处理空闲检测 (在主循环中调用)
 * Modbus RTU: 3.5字节时间无新数据视为帧结束
 * 115200下: 1字节≈87us, 3.5字节≈300us → 5ms超时足够
 */
void USART1_ProcessIdle(void)
{
    if (rx_busy && rx_count > 0 && (HAL_GetTick() - last_rx_tick) > IDLE_TIMEOUT_MS) {
        // 接收完成，调用回调处理Modbus帧
        if (rx_callback) {
            rx_callback(rx_buf, rx_count);
        }
        // 重置接收状态
        rx_count = 0;
        rx_busy = false;
        HAL_UART_Receive_IT(&huart1, &rx_buf[0], 1);
    }
}
