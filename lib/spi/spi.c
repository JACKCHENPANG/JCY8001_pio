/**
 * @file spi.c
 * @brief STM32 SPI1 HAL driver (polling + DMA)
 *
 * Ported from: Drivers/Driver_SPI1.c (Keil RTX → HAL)
 */

#include "spi.h"
#include "stm32f1xx_hal.h"

// SPI handle
static SPI_HandleTypeDef hspi1;

// DMA handles
static DMA_HandleTypeDef hdma_spi1_rx;
static DMA_HandleTypeDef hdma_spi1_tx;

void SPI1_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    // Enable clocks
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    // Configure NSS (PB2) as push-pull output HIGH
    GPIO_InitStruct.Pin   = SPI1_NSS_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(SPI1_NSS_PORT, &GPIO_InitStruct);
    SPI1_NSS_High();

    // Configure SCK (PA5), MISO (PA6), MOSI (PA7)
    // Remap: SPI1 uses default pins PA5/6/7, no remap needed
    GPIO_InitStruct.Pin   = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    GPIO_InitStruct.Mode  = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // SPI configuration: Mode 0 (CPOL=0, CPHA=0), MSB first, Software NSS
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize           = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity        = SPI_POLARITY_LOW;   // CPOL=0
    hspi1.Init.CLKPhase           = SPI_PHASE_1EDGE;     // CPHA=0
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64; // ~1.1MHz @ 72MHz
    hspi1.Init.FirstBit           = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial     = 7;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) {
        while (1);
    }

    __HAL_SPI_ENABLE(&hspi1);
}

void SPI1_NSS_Low(void)  { HAL_GPIO_WritePin(SPI1_NSS_PORT, SPI1_NSS_PIN, GPIO_PIN_RESET); }
void SPI1_NSS_High(void) { HAL_GPIO_WritePin(SPI1_NSS_PORT, SPI1_NSS_PIN, GPIO_PIN_SET); }

SPI_Status_t SPI1_WriteReg(uint16_t data, uint32_t len, SPI_WordWide_t wordWide)
{
    if (wordWide == SPI_WORD_16BIT) {
        hspi1.Init.DataSize = SPI_DATASIZE_16BIT;
    } else {
        hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    }
    if (HAL_SPI_Init(&hspi1) != HAL_OK) return SPI_ERR;

    HAL_SPI_Transmit(&hspi1, (uint8_t*)&data, len, 100);
    return SPI_OK;
}

SPI_Status_t SPI1_WriteBlock(void *txBuf, uint32_t txSize, SPI_WordWide_t wordWide, SPI_BufWide_t bufWide)
{
    uint8_t *p   = (uint8_t*)txBuf;
    uint32_t cnt = txSize;

    // 16-bit SPI data size
    if (wordWide == SPI_WORD_16BIT) {
        hspi1.Init.DataSize = SPI_DATASIZE_16BIT;
    } else {
        hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    }
    if (HAL_SPI_Init(&hspi1) != HAL_OK) return SPI_ERR;

    if (bufWide == SPI_BUF_U16) {
        // Convert 16-bit CPU buffer to 8-bit SPI bytes
        uint16_t *pw = (uint16_t*)txBuf;
        cnt = txSize * 2;
        p   = (uint8_t*)pw;
    }

    if (HAL_SPI_Transmit(&hspi1, p, cnt, 1000) != HAL_OK)
        return SPI_ERR;
    return SPI_OK;
}

SPI_Status_t SPI1_ReadBlock(void *rxBuf, uint32_t rxSize, SPI_WordWide_t wordWide, SPI_BufWide_t bufWide)
{
    uint8_t *p   = (uint8_t*)rxBuf;
    uint32_t cnt = rxSize;

    if (wordWide == SPI_WORD_16BIT) {
        hspi1.Init.DataSize = SPI_DATASIZE_16BIT;
    } else {
        hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    }
    if (HAL_SPI_Init(&hspi1) != HAL_OK) return SPI_ERR;

    if (bufWide == SPI_BUF_U16) {
        uint16_t *pw = (uint16_t*)rxBuf;
        cnt = rxSize * 2;
        p   = (uint8_t*)pw;
    }

    // Fill TX with 0xFF (dummy bytes) to clock in data
    if (HAL_SPI_Receive(&hspi1, p, cnt, 1000) != HAL_OK)
        return SPI_ERR;

    if (bufWide == SPI_BUF_U16) {
        uint16_t *pw = (uint16_t*)rxBuf;
        for (uint32_t i = 0; i < rxSize; i++) {
            pw[i] = (pw[i*2] << 8) | pw[i*2+1];
        }
    }
    return SPI_OK;
}

SPI_Status_t SPI1_FullDuplex(void *txBuf, void *rxBuf, uint32_t size, SPI_WordWide_t wordWide, SPI_BufWide_t bufWide)
{
    uint8_t *ptx = (uint8_t*)txBuf;
    uint8_t *prx = (uint8_t*)rxBuf;
    uint32_t cnt = size;

    if (wordWide == SPI_WORD_16BIT) {
        hspi1.Init.DataSize = SPI_DATASIZE_16BIT;
    } else {
        hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    }
    if (HAL_SPI_Init(&hspi1) != HAL_OK) return SPI_ERR;

    if (bufWide == SPI_BUF_U16) {
        uint16_t *pwu = (uint16_t*)txBuf;
        uint16_t *pwu2 = (uint16_t*)rxBuf;
        cnt = size * 2;
        ptx = (uint8_t*)pwu;
        prx = (uint8_t*)pwu2;
    }

    if (HAL_SPI_TransmitReceive(&hspi1, ptx, prx, cnt, 1000) != HAL_OK)
        return SPI_ERR;
    return SPI_OK;
}

/* ── DMA setup helpers ─────────────────────────────────────────── */

static void DMA1_CH2_3_Config(void)
{
    // DMA1 CH2 = SPI1_RX, DMA1 CH3 = SPI1_TX
    hdma_spi1_rx.Instance                 = DMA1_Channel2;
    hdma_spi1_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_spi1_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_spi1_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_spi1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_spi1_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_spi1_rx.Init.Mode                = DMA_NORMAL;
    hdma_spi1_rx.Init.Priority            = DMA_PRIORITY_LOW;
    __DMA1_FORCE_RESET();
    __DMA1_RELEASE_RESET();
    HAL_DMA_Init(&hdma_spi1_rx);
    __HAL_LINKDMA(&hspi1, hdmarx, hdma_spi1_rx);

    hdma_spi1_tx.Instance                 = DMA1_Channel3;
    hdma_spi1_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_spi1_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_spi1_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_spi1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_spi1_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_spi1_tx.Init.Mode                = DMA_NORMAL;
    hdma_spi1_tx.Init.Priority            = DMA_PRIORITY_LOW;
    __DMA1_FORCE_RESET();
    __DMA1_RELEASE_RESET();
    HAL_DMA_Init(&hdma_spi1_tx);
    __HAL_LINKDMA(&hspi1, hdmatx, hdma_spi1_tx);

    // Enable DMA interrupt
    HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);
    HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);
}

SPI_Status_t SPI1_DMA_WriteBlock(void *txBuf, uint32_t txSize, SPI_WordWide_t wordWide)
{
    hspi1.Init.DataSize = (wordWide == SPI_WORD_16BIT) ? SPI_DATASIZE_16BIT : SPI_DATASIZE_8BIT;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) return SPI_ERR;

    DMA1_CH2_3_Config();
    if (HAL_SPI_Transmit_DMA(&hspi1, (uint8_t*)txBuf, txSize) != HAL_OK)
        return SPI_ERR;
    return SPI_OK;
}

SPI_Status_t SPI1_DMA_ReadBlock(void *rxBuf, uint32_t rxSize, SPI_WordWide_t wordWide)
{
    hspi1.Init.DataSize = (wordWide == SPI_WORD_16BIT) ? SPI_DATASIZE_16BIT : SPI_DATASIZE_8BIT;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) return SPI_ERR;

    DMA1_CH2_3_Config();
    if (HAL_SPI_Receive_DMA(&hspi1, (uint8_t*)rxBuf, rxSize) != HAL_OK)
        return SPI_ERR;
    return SPI_OK;
}

SPI_Status_t SPI1_DMA_FullDuplex(void *txBuf, void *rxBuf, uint32_t size, SPI_WordWide_t wordWide)
{
    hspi1.Init.DataSize = (wordWide == SPI_WORD_16BIT) ? SPI_DATASIZE_16BIT : SPI_DATASIZE_8BIT;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) return SPI_ERR;

    DMA1_CH2_3_Config();
    if (HAL_SPI_TransmitReceive_DMA(&hspi1, (uint8_t*)txBuf, (uint8_t*)rxBuf, size) != HAL_OK)
        return SPI_ERR;
    return SPI_OK;
}

/* ── Interrupt handlers ─────────────────────────────────────── */

void DMA1_Channel2_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_spi1_rx); }
void DMA1_Channel3_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_spi1_tx); }
