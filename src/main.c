/*
 * main.c - JCY8001 Firmware (PlatformIO 移植版)
 *
 * 基于 jcy8001_firmware v0.6 移植到 PlatformIO
 *
 * 功能:
 *   - USART2 Modbus RTU 通讯
 *   - SPI1 DNB1101 阻抗测量
 *   - 定期轮询 DNB1101 数据更新到 Modbus 寄存器
 */

#include "../inc/register.h"
#include "../inc/modbus.h"
#include "../inc/usart.h"
#include "../inc/spi.h"
#include "../inc/stm32f1xx.h"

/* SystemInit 声明 - 在链接器脚本中定义 */
extern void SystemInit(void);

/* SysTick 延时计数 */
static volatile uint32_t g_systick = 0;

void SysTick_Handler(void) {
    g_systick++;
}

void delay_ms(uint32_t ms) {
    uint32_t start = g_systick;
    while ((g_systick - start) < ms);
}

/*
 * System clock initialization
 * 使用 HSE 外部晶振 (8MHz)
 * HSE -> PLL x9 -> 72MHz
 * AHB = 72MHz, APB1 = 36MHz, APB2 = 72MHz
 */
static void clock_init(void) {
    /* 使能HSE */
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY));

    /* PLL: HSE * 9 = 72MHz */
    RCC->CFGR = (RCC->CFGR & ~((0xFUL << 18) | (0x3UL << 0))) | (9UL << 18) | (2UL << 0);
    /* PLLSRC = HSE */
    RCC->CFGR |= (1UL << 16);

    /* 使能PLL */
    RCC->CR |= (1UL << 24);
    while (!(RCC->CR & (1UL << 25)));

    /* 切换系统时钟到PLL */
    RCC->CFGR = (RCC->CFGR & ~0x3UL) | (2UL << 0);
    while (((RCC->CFGR >> 2) & 0x3UL) != 2);

    /* 配置APB1分频 = /2 (36MHz) */
    RCC->CFGR = (RCC->CFGR & ~0x1C00UL) | (0x4UL << 8);  /* PPRE1 = 100 = /2 */

    /* SysTick: AHB / 8 = 9MHz, 9000 counts = 1ms */
    SysTick->LOAD = 9000 - 1;
    SysTick->CTRL = (1UL << 2) | (1UL << 1) | (1UL << 0);  /* AHB/8, enable, enable IRQ */
}

int main(void) {
    /* 设置向量表到SRAM (VECT_TAB_SRAM) */
    SCB->VTOR = SRAM_BASE;

    /* 初始化时钟 */
    clock_init();

    /* 初始化寄存器 */
    register_init();

    /* 初始化 USART2 (Modbus 通讯) */
    usart2_init(115200);

    /* 初始化 Modbus 协议栈 */
    modbus_init();

    /* 启用全局中断 */
    __asm__ __volatile__("cpsie i");

    /* 尝试读取DNB1101版本验证SPI通讯 */
    uint8_t version = 0;
    for (uint8_t i = 0; i < 3; i++) {
        if (dnb1101_get_version(&version) == 0) {
            break;  /* 成功读取，跳出循环 */
        }
        delay_ms(100);
    }
    (void)version;  /* 防止未使用警告 */

    /* 定期更新计数器 */
    uint32_t update_tick = 0;

    /* 主循环 */
    while (1) {
        /* 处理 Modbus 帧 */
        modbus_poll();

        /* 每100ms更新一次DNB1101数据 */
        if ((g_systick - update_tick) >= 100) {
            update_tick = g_systick;
            register_update_dnb1101();
        }
    }

    return 0;
}
