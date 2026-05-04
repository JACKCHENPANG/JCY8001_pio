/**
 * @file task_modbus.c
 * @brief Modbus RTU polling task
 *
 * Ported from: Threads/Modbus_Thread.c (Keil RTX → FreeRTOS)
 * Polls USART1 idle-detection for Modbus frames, processes requests,
 * stores results in shared data structures.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "Modbus.h"
#include "Driver_USART1.h"

extern UART_HandleTypeDef huart2;

void vTaskModbus(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        /* Poll for completed frame (idle detection) */
        extern void USART1_ProcessIdle(void);
        USART1_ProcessIdle();

        /* Allow other tasks to run */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
