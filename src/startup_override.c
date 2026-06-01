#include <stdint.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;

int main(void);

void Reset_Handler_impl(void);
void Default_Handler(void);
void HardFault_Handler(void);
void USART2_IRQHandler(void);   /* Modbus RX (defined in main.c) */

/*
 * Provide a complete vector table in .isr_vector.
 * Previous image had empty .isr_vector, causing reset to jump into garbage and
 * immediate HardFault/T-bit warning. This table guarantees valid SP/PC.
 *
 * Extended with the full STM32F103 peripheral IRQ table (60 entries) so the
 * USART2 RX interrupt (IRQ position 38) has a valid vector. Without these
 * entries, enabling the USART2 interrupt would fetch a garbage vector → HardFault.
 */
__attribute__((used, section(".isr_vector")))
void (* const g_pfnVectors[])(void) = {
    (void (*)(void))(&_estack), /* Initial stack pointer */
    Reset_Handler_impl,         /* Reset */
    Default_Handler,            /* NMI */
    HardFault_Handler,          /* HardFault */
    Default_Handler,            /* MemManage */
    Default_Handler,            /* BusFault */
    Default_Handler,            /* UsageFault */
    0, 0, 0, 0,                /* Reserved */
    Default_Handler,            /* SVCall */
    Default_Handler,            /* DebugMon */
    0,                          /* Reserved */
    Default_Handler,            /* PendSV */
    Default_Handler,            /* SysTick */
    /* ── External IRQs (position 0..59) ── */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 0  WWDG, PVD, TAMPER, RTC          */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 4  FLASH, RCC, EXTI0, EXTI1        */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 8  EXTI2, EXTI3, EXTI4, DMA1_1     */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 12 DMA1_2..5                       */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 16 DMA1_6,7, ADC1_2, USB_HP        */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 20 USB_LP, CAN_RX1, CAN_SCE, EXTI9_5 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 24 TIM1 BRK/UP/TRG/CC              */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 28 TIM2, TIM3, TIM4, I2C1_EV       */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 32 I2C1_ER, I2C2_EV, I2C2_ER, SPI1 */
    Default_Handler, Default_Handler, USART2_IRQHandler, Default_Handler, /* 36 SPI2, USART1, USART2(38), USART3 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 40 EXTI15_10, RTCAlarm, USBWakeup, TIM8_BRK */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 44 TIM8 UP/TRG/CC, ADC3            */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 48 FSMC, SDIO, TIM5, SPI3          */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 52 UART4, UART5, TIM6, TIM7        */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,   /* 56 DMA2_1, DMA2_2, DMA2_3, DMA2_4_5 */
};

void Reset_Handler_impl(void) {
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;

    while (dst < &_edata) {
        *dst++ = *src++;
    }

    for (dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0;
    }

    (void)main();

    while (1) {
        __asm volatile ("nop");
    }
}

void Default_Handler(void) {
    while (1) {
        __asm volatile ("nop");
    }
}

void HardFault_Handler(void) {
    while (1) {
        __asm volatile ("nop");
    }
}
