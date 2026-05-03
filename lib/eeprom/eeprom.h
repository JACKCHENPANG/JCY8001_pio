/**
 * @file eeprom.h
 * @brief AT24Cxx EEPROM driver (Software I2C)
 *
 * Hardware: PB6=SCL, PB7=SDA
 * Chip: AT24C128 (16K x 8, 64-byte page write, I2C @ 100/400kHz)
 *
 * Ported from: Drivers/Driver_24CXX.c (Keil RTX → HAL)
 * Note: Removed osDelay/osMutex calls — replaced with blocking waits.
 *       EEPROM_WriteByte uses HAL_Delay for write cycle time.
 */

#ifndef _EEPROM_HAL_H_
#define _EEPROM_HAL_H_

#include <stdint.h>

// AT24C128 configuration
#define EEPROM_TYPE             AT24C128
#define EEPROM_PAGE_SIZE        64
#define EEPROM_TOTAL_SIZE       (16 * 1024)   // 16384 bytes
#define EEPROM_USE_WORD_ADDR    0             // 0=device addr only; 1=device+word addr in device byte

// AT24C series helpers
#define AT24C01     127
#define AT24C02     255
#define AT24C04     511
#define AT24C08     1023
#define AT24C16     2047
#define AT24C32     4095
#define AT24C64     8191
#define AT24C128    16383
#define AT24C256    32767
#define AT24C512    65535

// Device base I2C address (A0/A1/A2 grounded)
#define EEPROM_I2C_ADDR   0x50   // 1010 000 + 0(write) / 1(read)

// Status codes
typedef enum {
    EEPROM_OK   = 0,
    EEPROM_ERR  = 1,
    EEPROM_BUSY = 2,
    EEPROM_TIMEOUT = 3,
} EEPROM_Status_t;

// EEPROM init — must be called before any read/write
void EEPROM_Init(void);

// Single byte operations
uint8_t  EEPROM_ReadByte(uint16_t addr);
void     EEPROM_WriteByte(uint16_t addr, uint8_t data);

// Block operations
uint16_t EEPROM_ReadBlock(uint16_t addr, uint8_t *buf, uint16_t len);
uint16_t EEPROM_WriteBlock(uint16_t addr, const uint8_t *buf, uint16_t len);

// Self-test (writes/reads magic pattern at end of memory)
uint8_t  EEPROM_Check(void);

// Query
uint8_t  EEPROM_GetStatus(void);
uint16_t EEPROM_GetTotalSize(void);
uint16_t EEPROM_GetPageSize(void);

#endif // _EEPROM_HAL_H_
