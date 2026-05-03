#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configUSE_PREEMPTION            1
#define configUSE_IDLE_HOOK             0
#define configUSE_TICK_HOOK             0
#define configCPU_CLOCK_HZ              ( 64000000UL )
#define configTICK_RATE_HZ              ( ( int32_t ) 1000 )
#define configMINIMAL_STACK_SIZE        ( ( uint16_t ) 128 )
#define configTOTAL_HEAP_SIZE           ( ( size_t ) 8192 )
#define configMAX_PRIORITIES            5
#define configSUPPORT_STATIC_ALLOCATION 0
#define configUSE_MUTEXES               1
#define configUSE_TRACE_FACILITY        0
#define configUSE_16_BIT_TICKS         0
#define configMAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY  15

/* CMSIS-RTOS hook */
#define configUSE_MALLOC_FAILED_HOOK    0

#define INCLUDE_vTaskDelete             1
#define INCLUDE_vTaskDelayUntil         1
#define INCLUDE_vTaskDelay              1
#define INCLUDE_uxTaskGetStackHighWaterMark 1

#endif /* FREERTOS_CONFIG_H */
