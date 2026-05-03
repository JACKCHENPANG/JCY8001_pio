/*
 * crc16.c - Modbus CRC16 calculation
 *
 * JCY8001 PlatformIO Project
 *
 * Polynomial: 0xA001 (标准 Modbus)
 * Initial value: 0xFFFF
 */

#include "../inc/crc16.h"

uint16_t crc16_modbus(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;

    while (length--) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}
