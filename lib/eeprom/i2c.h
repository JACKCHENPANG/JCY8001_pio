/**
 * @file i2c.h
 * @brief Software I2C driver for STM32F1xx HAL
 *
 * Hardware: PB6=SCL, PB7=SDA (Open-drain, external pull-up required)
 * Ported from: Drivers/Driver_IIC.c (Keil RTX → HAL)
 */

#ifndef _I2C_HAL_H_
#define _I2C_HAL_H_

#include <stdint.h>

// Status codes
#define I2C_OK   1
#define I2C_ERR  0

/**
 * Initialize I2C GPIO (PB6=SCL, PB7=SDA)
 * Call once at startup.
 */
void I2C_Init(void);

/**
 * Generate I2C START condition
 */
void I2C_Start(void);

/**
 * Generate I2C STOP condition
 */
void I2C_Stop(void);

/**
 * Send one byte, return 1 if ACK received, 0 if NACK/timeout
 */
uint8_t I2C_Write(uint8_t txd);

/**
 * Read one byte, send ack/nack based on last parameter
 * @param ack  1=send ACK (continue reading), 0=send NACK (stop reading)
 */
uint8_t I2C_Read(uint8_t ack);

#endif // _I2C_HAL_H_
