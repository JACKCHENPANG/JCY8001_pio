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

/*
 * Provide a complete vector table in .isr_vector.
 * Previous image had empty .isr_vector, causing reset to jump into garbage and
 * immediate HardFault/T-bit warning. This table guarantees valid SP/PC.
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
