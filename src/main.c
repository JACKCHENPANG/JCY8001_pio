/*
 * main.c - JCY8001 Firmware (PlatformIO 移植版)
 *
 * SPI Daisy-Chain 验证版本 — 枚举 DNB1101 + 持续 SPI 发送供示波器观察
 *
 * 功能:
 *   - SPI1 DNB1101 daisy-chain 协议枚举
 *   - USART2 Modbus RTU 通讯 (轮询)
 */

#include "../inc/register.h"
#include "../inc/modbus.h"
#include "../inc/usart.h"
#include "../inc/spi.h"
#include "../inc/stm32f1xx.h"

extern void SystemInit(void);

static volatile uint32_t g_systick = 0;

void SysTick_Handler(void) { g_systick++; }

void delay_ms(uint32_t ms) {
    uint32_t start = g_systick;
    while ((g_systick - start) < ms);
}

static void clock_init(void) {
    RCC->CFGR = (RCC->CFGR & ~0xFUL) | (0UL << 0);
    while (((RCC->CFGR >> 2) & 0x3UL) != 0);
    SysTick->LOAD = 1000 - 1;
    SysTick->CTRL = (1UL << 2) | (1UL << 1) | (1UL << 0);
}

int main(void) {
    clock_init();
    register_init();
    usart2_init(115200);
    modbus_init();

    /* SPI1 init 必须在所有 GPIOA 操作之后！
     * usart2_init() 也写 GPIOA_CRL (PA2/PA3)，确保 SPI 引脚配置是最后一次 */
    spi1_init();
    __asm__ __volatile__("cpsie i");

    /* ── DNB1101 Daisy-Chain 枚举 ── */
    uint8_t ic_ids[DNB11XX_MAX_CHAIN];
    uint8_t ic_count = 0;

    /* 等待 DNB1101 上电稳定 */
    delay_ms(50);

    for (int attempt = 0; attempt < 5; attempt++) {
        ic_count = dnb1101_chain_enumerate(ic_ids, DNB11XX_MAX_CHAIN);
        if (ic_count > 0) break;
        delay_ms(100);
    }

    /* 枚举成功后发送 Init + SetMode */
    if (ic_count > 0) {
        dnb1101_chain_init();
    }

    uint32_t last_spi = g_systick;
    uint32_t last_modbus = g_systick;
    uint8_t data_type = 0;  /* 轮询 GetData 类型 */

    /* ── 主循环 ── */
    while (1) {
        /* Modbus 轮询 */
        if ((g_systick - last_modbus) >= 5) {
            last_modbus = g_systick;
            modbus_poll();
        }

        /* 每 50ms 做一次 SPI 操作 (提高频率以便示波器捕获) */
        if ((g_systick - last_spi) >= 50) {
            last_spi = g_systick;

            if (ic_count > 0) {
                /* GetData 轮询: MainVolt(0) → Temp(2) → ProductVer(15) 循环 */
                uint8_t resp[4];
                uint8_t dt = 0;
                switch (data_type) {
                    case 0: dt = 0x00; break;  /* MainVolt */
                    case 1: dt = 0x02; break;  /* MainCellTemp */
                    case 2: dt = 0x0F; break;  /* ProductVer */
                }
                dnb1101_chain_get_data(ic_ids[0], dt, resp);
                data_type = (data_type + 1) % 3;
            } else {
                /* 无 IC 发现 → 重新枚举 */
                ic_count = dnb1101_chain_enumerate(ic_ids, DNB11XX_MAX_CHAIN);
            }
        }
    }

    return 0;
}
