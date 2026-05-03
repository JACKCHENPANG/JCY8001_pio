/**
 * @file crc.h
 * @brief DNB11xx CRC utilities
 *
 * DNB11xx uses a 4-bit CRC with polynomial x^3 + x^0 = 0x09.
 * Two implementations: table lookup (fast) and bit-by-bit.
 *
 * Ported from: Sources/CRC.c
 */

#ifndef _CRC_HAL_H_
#define _CRC_HAL_H_

#include <stdint.h>

/**
 * Compute 4-bit CRC using lookup table (fast path).
 * Used by DNB11xx protocol for all command/data words.
 */
uint8_t CRC4_TableMode(uint8_t *p, uint32_t length);

/**
 * Compute 4-bit CRC bit-by-bit (slow path, reference implementation).
 */
uint8_t CRC4(uint8_t *p, uint32_t length);

/**
 * Compute 4-bit CRC variant 1 (XOR-shift).
 */
uint8_t CRC4_1(uint8_t *p, uint32_t length);

#endif // _CRC_HAL_H_
