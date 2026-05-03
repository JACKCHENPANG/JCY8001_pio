/**
 * USART1驱动 - HAL库版本
 * 用于Modbus通信
 */

#ifndef _DRIVER_USART1_H_
#define _DRIVER_USART1_H_

#include <stdint.h>
#include <stdbool.h>
#include "stm32f1xx_hal.h"

// 外部变量声明
extern UART_HandleTypeDef huart1;

// 函数声明
void USART1_Init(void);
void USART1_Send(uint8_t *p, uint16_t Length, bool isWaiting);
void USART1_SetBaudRate(uint32_t baudrate);

// 接收回调函数类型
typedef void (*USART1_RxCallback_t)(uint8_t *data, uint16_t length);
void USART1_SetRxCallback(USART1_RxCallback_t callback);

// 空闲检测处理 (主循环调用)
void USART1_ProcessIdle(void);

#endif /* _DRIVER_USART1_H_ */
