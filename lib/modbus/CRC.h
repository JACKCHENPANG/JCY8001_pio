/**
 * CRC计算模块
 * 移植自原始Keil工程
 */

#ifndef _CRC_H_
#define _CRC_H_

#include <stdint.h>

// CRC4计算
uint8_t CRC4_TableMode(uint8_t *p, uint32_t length);
uint8_t CRC4(uint8_t *p, uint32_t length);
uint8_t CRC4_1(uint8_t *p, uint32_t length);

// CRC16计算 (Modbus用)
uint16_t Modbus_CRC16(uint8_t *pFram, uint16_t len);
uint16_t Modbus_CRC16_Lookup(uint8_t *pFram, uint16_t len);

#endif /* _CRC_H_ */
