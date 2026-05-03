/**
 * @file i2c.c
 * @brief Software I2C driver for STM32F1xx HAL
 *
 * Hardware: PB6=SCL, PB7=SDA (Open-drain, external pull-up required)
 * Ported from: Drivers/Driver_IIC.c (Keil RTX → HAL)
 */

#include "i2c.h"
#include "stm32f1xx_hal.h"

// GPIO port and pin definitions
#define I2C_SCL_PORT  GPIOB
#define I2C_SCL_PIN    GPIO_PIN_6
#define I2C_SDA_PORT   GPIOB
#define I2C_SDA_PIN    GPIO_PIN_7

// Bit-bang delay (adjust for your bus speed)
#define I2C_DELAY()    do { for(volatile uint32_t i = 0; i < 6; i++) __NOP(); } while(0)

// Helper macros
#define SDA_HIGH()  HAL_GPIO_WritePin(I2C_SDA_PORT, I2C_SDA_PIN, GPIO_PIN_SET)
#define SDA_LOW()   HAL_GPIO_WritePin(I2C_SDA_PORT, I2C_SDA_PIN, GPIO_PIN_RESET)
#define SCL_HIGH()  HAL_GPIO_WritePin(I2C_SCL_PORT, I2C_SCL_PIN, GPIO_PIN_SET)
#define SCL_LOW()   HAL_GPIO_WritePin(I2C_SCL_PORT, I2C_SCL_PIN, GPIO_PIN_RESET)
#define SDA_READ()  HAL_GPIO_ReadPin(I2C_SDA_PORT, I2C_SDA_PIN)

void I2C_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    // Enable GPIOB clock
    __HAL_RCC_GPIOB_CLK_ENABLE();

    // SCL: Push-pull output HIGH (drives high when no device pulls down)
    GPIO_InitStruct.Pin   = I2C_SCL_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(I2C_SCL_PORT, &GPIO_InitStruct);

    // SDA: Open-drain output HIGH (device can pull low)
    GPIO_InitStruct.Pin  = I2C_SDA_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(I2C_SDA_PORT, &GPIO_InitStruct);

    // Idle state: both lines high
    SCL_HIGH();
    SDA_HIGH();
}

void I2C_Start(void)
{
    SDA_HIGH();
    SCL_HIGH();
    I2C_DELAY();
    SDA_LOW();
    I2C_DELAY();
    SCL_LOW();   // SCL low locks the bus
    I2C_DELAY();
}

void I2C_Stop(void)
{
    SDA_LOW();
    I2C_DELAY();
    SCL_HIGH();
    I2C_DELAY();
    SDA_HIGH();  // SDA rising edge while SCL is high = STOP
    I2C_DELAY();
}

uint8_t I2C_Write(uint8_t txd)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        SCL_LOW();
        if (txd & 0x80)
            SDA_HIGH();
        else
            SDA_LOW();
        txd <<= 1;
        I2C_DELAY();
        SCL_HIGH();
        I2C_DELAY();
        SCL_LOW();
        I2C_DELAY();
    }

    // Release SDA for ACK bit
    SDA_HIGH();
    I2C_DELAY();
    SCL_HIGH();

    uint8_t ack = !SDA_READ();  // ACK = SDA pulled low by device
    I2C_DELAY();
    SCL_LOW();
    I2C_DELAY();

    return ack;
}

uint8_t I2C_Read(uint8_t ack)
{
    uint8_t i;
    uint8_t receive = 0;

    // Release SDA so device can drive it
    SDA_HIGH();
    I2C_DELAY();

    for (i = 0; i < 8; i++) {
        SCL_LOW();
        I2C_DELAY();
        receive <<= 1;
        SCL_HIGH();
        I2C_DELAY();
        if (SDA_READ())
            receive |= 1;
    }
    SCL_LOW();
    I2C_DELAY();

    // Send ACK/NACK
    if (ack)
        SDA_LOW();   // ACK: continue reading
    else
        SDA_HIGH();  // NACK: stop reading
    I2C_DELAY();
    SCL_HIGH();
    I2C_DELAY();
    SCL_LOW();
    I2C_DELAY();
    SDA_HIGH();  // Release SDA

    return receive;
}
