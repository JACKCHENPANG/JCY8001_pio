/**
 * @file task_measure.h
 * @brief Measurement task
 */

#ifndef _TASK_MEASURE_H_
#define _TASK_MEASURE_H_

#include <stdint.h>

void vTaskMeasure(void *pvParameters);

/* Read shared data (safe to call from ISR/task) */
uint8_t  Measure_GetCount(void);
uint16_t Measure_GetVoltage(uint8_t ic_idx);
int16_t  Measure_GetTemperature(uint8_t ic_idx);
uint16_t Measure_GetZreal(uint8_t ic_idx);
uint16_t Measure_GetZimag(uint8_t ic_idx);
uint16_t Measure_GetZMV(uint8_t ic_idx);
uint8_t  Measure_IsValid(uint8_t ic_idx);
uint32_t Measure_GetTick500ms(void);

#endif
