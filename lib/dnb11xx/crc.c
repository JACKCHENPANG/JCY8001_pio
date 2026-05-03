/**
 * @file crc.c
 * @brief DNB11xx CRC utilities
 *
 * DNB11xx uses a 4-bit CRC with polynomial x^3 + x^0 = 0x09.
 *
 * Ported from: Sources/CRC.c
 */

#include "crc.h"

static const uint8_t CRC4_Table[16] = {
    0x00, 0x5e, 0xbc, 0xe2,
    0x61, 0x3f, 0xdd, 0x83,
    0xc2, 0x9c, 0x7e, 0x20,
    0xa3, 0xfd, 0x1f, 0x41,
};

uint8_t CRC4_TableMode(uint8_t *p, uint32_t length)
{
    uint8_t crc = 0;
    for (uint32_t i = 0; i < length; i++) {
        crc = CRC4_Table[crc ^ p[i]];
    }
    return crc;
}

uint8_t CRC4(uint8_t *p, uint32_t length)
{
    uint8_t crc = 0;
    for (uint32_t i = 0; i < length; i++) {
        crc = p[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc <<= 1;
        }
    }
    return crc;
}

uint8_t CRC4_1(uint8_t *p, uint32_t length)
{
    uint8_t crc = 0x0;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= p[i];
        crc  = (crc << 1) & 0xf8;
    }
    return crc;
}
