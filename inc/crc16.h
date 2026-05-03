/**
 * crc16.h - Modbus CRC16 calculation
 *
 * JCY8001 PlatformIO Project
 */

#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>
#include <stddef.h>

/**
 * Calculate Modbus CRC16
 * Polynomial: 0xA001 (standard Modbus)
 * Initial value: 0xFFFF
 */
uint16_t crc16_modbus(const uint8_t *data, size_t length);

#endif /* CRC16_H */
