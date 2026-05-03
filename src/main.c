/**
 * JCY8001 阻抗分析仪固件
 * PlatformIO移植版本
 *
 * Stage 0: LED闪烁    [完成]
 * Stage 1: Modbus     [完成]
 * Stage 2: EEPROM     [完成]
 * Stage 3: SPI+ADC    [完成]
 * Stage 4: FreeRTOS   [进行中]
 */

#include <stdio.h>
#include <string.h>
#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "Modbus.h"
#include "Driver_USART1.h"
#include "eeprom.h"
#include "spi.h"
#include "dnb11xx.h"
#include "tasks/task_modbus.h"
#include "tasks/task_measure.h"

// 系统时钟配置
void SystemClock_Config(void);
// GPIO初始化
static void MX_GPIO_Init(void);
// USART1初始化
static void MX_USART1_UART_Init(void);

// UART句柄
UART_HandleTypeDef huart1;

// Stage 3: DNB11xx IC数量
uint8_t dnb_ic_count = 0;

// 模拟寄存器数据 (测试用)
static uint16_t test_regs[64] = {
    0x0001,  // 版本号
    0x0002,
    0xEA60,  // 电压值 (60000)
    0x0136,  // 温度值 (310)
};

/**
 * Modbus读取寄存器回调
 */
static uint16_t modbus_read_reg(uint16_t addr)
{
    // 0x3E00 - IC数量 (DNB11xx)
    if (addr == 0x3E00) return dnb_ic_count;
    // 0x3E01 - 版本号
    if (addr == 0x3E01) return 1;
    // 0x3E02 - fw_version
    if (addr == 0x3E02) return 0x0200;
    // 0x3E03 - git_rev (placeholder)
    if (addr == 0x3E03) return 0;
    // 0x3E04 - build_date (placeholder)
    if (addr == 0x3E04) return 0x0503;

    // 0x3300-0x333F - 温度 (0.1°C units, signed)
    if (addr >= 0x3300 && addr < 0x3340) {
        uint8_t idx = (addr - 0x3300) / 2;
        if (idx < dnb_ic_count)
            return (uint16_t)Measure_GetTemperature(idx);
        return 0;
    }
    // 0x3340-0x337F - 电压 (mV)
    if (addr >= 0x3340 && addr < 0x3380) {
        uint8_t idx = (addr - 0x3340) / 2;
        if (idx < dnb_ic_count)
            return Measure_GetVoltage(idx);
        return 0;
    }
    // 0x3380-0x33BF - ZM状态 (valid flag)
    if (addr >= 0x3380 && addr < 0x33C0) {
        uint8_t idx = (addr - 0x3380) / 2;
        if (idx < dnb_ic_count)
            return Measure_IsValid(idx) ? 1 : 0;
        return 0;
    }

    // 0x4000-0x403F - Zreal (mantissa | exponent<<12)
    if (addr >= 0x4000 && addr < 0x4040) {
        uint8_t idx = (addr - 0x4000) / 2;
        if (idx < dnb_ic_count)
            return Measure_GetZreal(idx);
        return 0;
    }
    // 0x4040-0x407F - Zimag
    if (addr >= 0x4040 && addr < 0x4080) {
        uint8_t idx = (addr - 0x4040) / 2;
        if (idx < dnb_ic_count)
            return Measure_GetZimag(idx);
        return 0;
    }
    // 0x4080-0x40BF - ZM voltage
    if (addr >= 0x4080 && addr < 0x40C0) {
        uint8_t idx = (addr - 0x4080) / 2;
        if (idx < dnb_ic_count)
            return Measure_GetZMV(idx);
        return 0;
    }

    // 0x3E10-0x3E1F - DNB11xx 数据 (voltage + temp, for backwards compat)
    if (addr >= 0x3E10 && addr < 0x3E20) {
        if (dnb_ic_count > 0) {
            uint8_t idx = (addr - 0x3E10) / 2;
            if (idx < dnb_ic_count) {
                if ((addr & 0x01) == 0)
                    return Measure_GetVoltage(idx);
                else
                    return (uint16_t)Measure_GetTemperature(idx);
            }
        }
        return 0;
    }

    // 其他地址返回0
    if (addr < 64) return test_regs[addr];
    return 0;
}

/**
 * Modbus写入寄存器回调
 */
static void modbus_write_reg(uint16_t addr, uint16_t value)
{
    if (addr < 64) {
        test_regs[addr] = value;
        // 自动写入EEPROM (可选功能)
        // EEPROM_WriteBlock(addr * 2, (uint8_t*)&value, 2);
    }
}

/**
 * USART1接收回调
 */
static void usart1_rx_callback(uint8_t *data, uint16_t len)
{
    // 处理Modbus请求
    Modbus_Process(data, len);
}

/**
 * 打印EEPROM自检结果
 */
static void eeprom_report_status(void)
{
    uint8_t st = EEPROM_GetStatus();
    char buf[64];
    int len;

    switch (st) {
        case 0:  len = snprintf(buf, sizeof(buf), "EEPROM: OK (0x55 found)\r\n"); break;
        case 1:  len = snprintf(buf, sizeof(buf), "EEPROM: ERR (write/read mismatch)\r\n"); break;
        case 2:  len = snprintf(buf, sizeof(buf), "EEPROM: NO CHIP (not found)\r\n"); break;
        default: len = snprintf(buf, sizeof(buf), "EEPROM: UNKNOWN status=%d\r\n", st); break;
    }

    HAL_UART_Transmit(&huart1, (uint8_t*)buf, len, 100);
}

/**
 * 打印DNB11xx枚举结果
 */
static void dnb_report_status(void)
{
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "DNB11xx: ICs=%d\r\n", dnb_ic_count);
    HAL_UART_Transmit(&huart1, (uint8_t*)buf, len, 100);
}

int main(void)
{
    // HAL库初始化
    HAL_Init();

    // 系统时钟配置
    SystemClock_Config();

    // GPIO初始化
    MX_GPIO_Init();

    // USART1初始化
    MX_USART1_UART_Init();

    // 初始化USART1驱动
    USART1_Init();
    USART1_SetRxCallback(usart1_rx_callback);

    // 初始化Modbus
    Modbus_Init();
    Modbus_SetReadCallback(modbus_read_reg);
    Modbus_SetWriteCallback(modbus_write_reg);

    // [Stage 2] 初始化EEPROM
    EEPROM_Init();
    eeprom_report_status();

    // [Stage 3] 初始化SPI+DNB11xx
    // DNB11xx_Init/Enumerate 在 task_measure 中执行，避免重复调用
    extern void DNB11xx_Init(void);
    extern uint8_t DNB11xx_Enumerate(void);
    (void)DNB11xx_Init();
    dnb_ic_count = DNB11xx_Enumerate();
    dnb_report_status();

    // [Stage 4] 创建FreeRTOS任务并启动调度器
    BaseType_t ret;
    ret = xTaskCreate(vTaskModbus, "Modbus", 256, NULL, 2, NULL);
    (void)ret;
    ret = xTaskCreate(vTaskMeasure, "Measure", 512, NULL, 3, NULL);
    (void)ret;

    // 发送启动消息
    const char *msg = "JCY8001 FreeRTOS Starting\r\n";
    HAL_UART_Transmit(&huart1, (uint8_t*)msg, strlen(msg), 100);

    // 启动FreeRTOS调度器 (永不返回)
    vTaskStartScheduler();

    // 如果调度器返回(不应发生)，进入死循环
    for (;;) {}
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    // 使用HSI内部振荡器 (64MHz)
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL16;  // 8MHz / 2 * 16 = 64MHz
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        while(1);
    }

    // 配置系统时钟
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                                  |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        while(1);
    }
}

static void MX_USART1_UART_Init(void)
{
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK)
    {
        while(1);
    }
}

static void MX_GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
}

/**
 * HAL UART MSP初始化回调
 */
void HAL_UART_MspInit(UART_HandleTypeDef* huart)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (huart->Instance == USART1)
    {
        // 使能时钟
        __HAL_RCC_USART1_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();

        // TX: PA9 - 复用推挽输出
        GPIO_InitStruct.Pin = GPIO_PIN_9;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        // RX: PA10 - 浮空输入
        GPIO_InitStruct.Pin = GPIO_PIN_10;
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        // 使能接收中断
        HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
        HAL_NVIC_EnableIRQ(USART1_IRQn);
    }
}

/**
 * SysTick中断处理
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/**
 * USART1中断处理
 */
void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart1);
}
