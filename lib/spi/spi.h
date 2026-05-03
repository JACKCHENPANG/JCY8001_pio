/**
 * @file spi.h
 * @brief STM32 SPI1 HAL driver (polling + DMA)
 *
 * Hardware: SPI1 on APB2 (72MHz)
 *   SCK  = PA5
 *   MISO = PA6
 *   MOSI = PA7
 *   NSS  = PB2 (software GPIO, push-pull output)
 *
 * Ported from: Drivers/Driver_SPI1.c (Keil RTX → HAL)
 * Features:
 *   - Polling TX/RX/block transfers
 *   - DMA TX/RX (DMA1 CH2=rx, CH3=tx)
 *   - 8/16-bit word sizes
 */

#ifndef _SPI_HAL_H_
#define _SPI_HAL_H_

#include <stdint.h>

// Status codes
typedef enum {
    SPI_OK   = 0,
    SPI_ERR  = 1,
    SPI_BUSY = 2,
} SPI_Status_t;

// SPI word width
typedef enum {
    SPI_WORD_8BIT  = 0,
    SPI_WORD_16BIT = 1,
} SPI_WordWide_t;

// Buffer word width (CPU bus)
typedef enum {
    SPI_BUF_U8  = 0,
    SPI_BUF_U16 = 1,
} SPI_BufWide_t;

// SPI1 GPIO/NSS pin
#define SPI1_NSS_PORT  GPIOB
#define SPI1_NSS_PIN   GPIO_PIN_2

/**
 * Initialize SPI1 peripheral
 *   Mode 0 (CPOL=0, CPHA=0), MSB first, 1MHz baud, 8-bit
 */
void SPI1_Init(void);

/**
 * Assert NSS (pull low)
 */
void SPI1_NSS_Low(void);

/**
 * Deassert NSS (pull high)
 */
void SPI1_NSS_High(void);

/* ── Polling transfers ─────────────────────────────────────────── */

/**
 * Write raw register to SPI device (no NSS control)
 */
SPI_Status_t SPI1_WriteReg(uint16_t data, uint32_t len, SPI_WordWide_t wordWide);

/**
 * Write block to SPI device
 */
SPI_Status_t SPI1_WriteBlock(void *txBuf, uint32_t txSize, SPI_WordWide_t wordWide, SPI_BufWide_t bufWide);

/**
 * Read block from SPI device
 */
SPI_Status_t SPI1_ReadBlock(void *rxBuf, uint32_t rxSize, SPI_WordWide_t wordWide, SPI_BufWide_t bufWide);

/**
 * Full-duplex: TX + RX simultaneously
 */
SPI_Status_t SPI1_FullDuplex(void *txBuf, void *rxBuf, uint32_t size, SPI_WordWide_t wordWide, SPI_BufWide_t bufWide);

/* ── DMA transfers ───────────────────────────────────────────── */

/**
 * DMA: write block
 */
SPI_Status_t SPI1_DMA_WriteBlock(void *txBuf, uint32_t txSize, SPI_WordWide_t wordWide);

/**
 * DMA: read block
 */
SPI_Status_t SPI1_DMA_ReadBlock(void *rxBuf, uint32_t rxSize, SPI_WordWide_t wordWide);

/**
 * DMA: full duplex
 */
SPI_Status_t SPI1_DMA_FullDuplex(void *txBuf, void *rxBuf, uint32_t size, SPI_WordWide_t wordWide);

#endif // _SPI_HAL_H_
