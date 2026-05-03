/**
 * stm32f1xx.h - Minimal STM32F103 register definitions
 * For JCY8001 PlatformIO project
 */

#ifndef STM32F1XX_H
#define STM32F1XX_H

#include <stdint.h>

/* ========================================================================
 * Base Addresses
 * ======================================================================== */
#define FLASH_BASE      0x08000000UL
#define SRAM_BASE       0x20000000UL
#define PERIPH_BASE     0x40000000UL
#define APB1_BASE       (PERIPH_BASE + 0x00000000UL)
#define APB2_BASE       (PERIPH_BASE + 0x00010000UL)
#define AHBPERIPH_BASE  (PERIPH_BASE + 0x00020000UL)

/* ========================================================================
 * RCC Registers
 * ======================================================================== */
#define RCC_BASE        (AHBPERIPH_BASE + 0x1000UL)
typedef struct {
    volatile uint32_t CR;
    volatile uint32_t CFGR;
    volatile uint32_t CIR;
    volatile uint32_t APB2RSTR;
    volatile uint32_t APB1RSTR;
    volatile uint32_t AHBENR;
    volatile uint32_t APB2ENR;
    volatile uint32_t APB1ENR;
    volatile uint32_t BDCR;
    volatile uint32_t CSR;
} RCC_TypeDef;

#define RCC             ((RCC_TypeDef *)RCC_BASE)

/* RCC_CR */
#define RCC_CR_HSEON        (1UL << 16)
#define RCC_CR_HSERDY       (1UL << 17)
#define RCC_CR_PLLON        (1UL << 24)
#define RCC_CR_PLLRDY       (1UL << 25)

/* RCC_CFGR */
#define RCC_CFGR_SW_HSE     (1UL << 0)
#define RCC_CFGR_SW_PLL     (2UL << 0)
#define RCC_CFGR_SWS_HSE    (1UL << 2)
#define RCC_CFGR_SWS_PLL    (2UL << 2)
#define RCC_CFGR_PLLMUL     (0xFUL << 18)
#define RCC_CFGR_PLLSRC     (1UL << 16)
#define RCC_CFGR_PPRE1      (7UL << 8)
#define RCC_CFGR_PPRE2      (7UL << 11)

/* RCC_APB2ENR */
#define RCC_APB2ENR_IOPAEN   (1UL << 2)
#define RCC_APB2ENR_IOPBEN   (1UL << 3)
#define RCC_APB2ENR_IOPCEN   (1UL << 4)
#define RCC_APB2ENR_SPI1EN   (1UL << 12)
#define RCC_APB2ENR_USART1EN (1UL << 14)
#define RCC_APB2ENR_ADC1EN   (1UL << 9)

/* RCC_APB1ENR */
#define RCC_APB1ENR_USART2EN (1UL << 17)
#define RCC_APB1ENR_SPI2EN   (1UL << 14)
#define RCC_APB1ENR_SPI3EN   (1UL << 15)
#define RCC_APB1ENR_TIM2EN   (1UL << 0)
#define RCC_APB1ENR_TIM3EN   (1UL << 1)
#define RCC_APB1ENR_TIM4EN   (1UL << 2)
#define RCC_APB1ENR_DMA1EN   (1UL << 0)

/* ========================================================================
 * GPIO Registers
 * ======================================================================== */
#define GPIOA_BASE      (APB2_BASE + 0x0000UL)
#define GPIOB_BASE      (APB2_BASE + 0x0400UL)
#define GPIOC_BASE      (APB2_BASE + 0x0800UL)

typedef struct {
    volatile uint32_t CRL;
    volatile uint32_t CRH;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t BRR;
    volatile uint32_t LCKR;
} GPIO_TypeDef;

#define GPIOA           ((GPIO_TypeDef *)GPIOA_BASE)
#define GPIOB           ((GPIO_TypeDef *)GPIOB_BASE)
#define GPIOC           ((GPIO_TypeDef *)GPIOC_BASE)

/* GPIO CRL/CRH modes */
#define GPIO_CRL_MODE_INPUT      0x0UL
#define GPIO_CRL_MODE_OUTPUT_10  0x1UL
#define GPIO_CRL_MODE_OUTPUT_2   0x2UL
#define GPIO_CRL_MODE_OUTPUT_50  0x3UL

#define GPIO_CRL_CNF_INPUT_ANALOG     0x0UL
#define GPIO_CRL_CNF_INPUT_FLOATING   0x4UL
#define GPIO_CRL_CNF_INPUT_PULL       0x8UL
#define GPIO_CRL_CNF_OUTPUT_PP        0x0UL
#define GPIO_CRL_CNF_OUTPUT_OD        0x4UL
#define GPIO_CRL_CNF_ALT_PP           0x8UL
#define GPIO_CRL_CNF_ALT_OD           0xCUL

/* ========================================================================
 * SPI Registers
 * ======================================================================== */
#define SPI1_BASE       (APB2_BASE + 0x3000UL)
#define SPI2_BASE       (APB1_BASE + 0x3000UL)
#define SPI3_BASE       (APB1_BASE + 0x3800UL)

typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t CRCPR;
    volatile uint32_t RXCRCR;
    volatile uint32_t TXCRCR;
} SPI_TypeDef;

#define SPI1            ((SPI_TypeDef *)SPI1_BASE)
#define SPI2            ((SPI_TypeDef *)SPI2_BASE)
#define SPI3            ((SPI_TypeDef *)SPI3_BASE)

/* SPI_CR1 */
#define SPI_CR1_CPHA        (1UL << 0)
#define SPI_CR1_CPOL        (1UL << 1)
#define SPI_CR1_MSTR        (1UL << 2)
#define SPI_CR1_BR_2        (1UL << 3)
#define SPI_CR1_BR_1        (1UL << 4)
#define SPI_CR1_BR_0        (1UL << 5)
#define SPI_CR1_SPE         (1UL << 6)
#define SPI_CR1_LSBFIRST    (1UL << 7)
#define SPI_CR1_SSI         (1UL << 8)
#define SPI_CR1_SSM         (1UL << 9)
#define SPI_CR1_RXONLY      (1UL << 10)
#define SPI_CR1_DFF         (1UL << 11)
#define SPI_CR1_CRCNEXT     (1UL << 12)
#define SPI_CR1_CRCEN       (1UL << 13)
#define SPI_CR1_BIDIOE      (1UL << 14)
#define SPI_CR1_BIDIMODE    (1UL << 15)

#define SPI_CR1_BR_DIV2     0x0UL
#define SPI_CR1_BR_DIV4     (1UL << 3)
#define SPI_CR1_BR_DIV8     (1UL << 4)
#define SPI_CR1_BR_DIV16    (1UL << 3 | 1UL << 4)
#define SPI_CR1_BR_DIV32    (1UL << 5)
#define SPI_CR1_BR_DIV64    (1UL << 3 | 1UL << 5)
#define SPI_CR1_BR_DIV128   (1UL << 4 | 1UL << 5)
#define SPI_CR1_BR_DIV256   (1UL << 3 | 1UL << 4 | 1UL << 5)

/* SPI_SR */
#define SPI_SR_RXNE         (1UL << 0)
#define SPI_SR_TXE          (1UL << 1)
#define SPI_SR_BSY          (1UL << 7)
#define SPI_SR_MODF         (1UL << 5)
#define SPI_SR_OVR          (1UL << 6)

/* ========================================================================
 * USART Registers
 * ======================================================================== */
#define USART1_BASE     (APB2_BASE + 0x3800UL)
#define USART2_BASE     (APB1_BASE + 0x4400UL)
#define USART3_BASE     (APB1_BASE + 0x4800UL)

typedef struct {
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t BRR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t GTPR;
} USART_TypeDef;

#define USART1          ((USART_TypeDef *)USART1_BASE)
#define USART2          ((USART_TypeDef *)USART2_BASE)
#define USART3          ((USART_TypeDef *)USART3_BASE)

/* USART_SR */
#define USART_SR_TXE        (1UL << 7)
#define USART_SR_RXNE       (1UL << 5)
#define USART_SR_TC         (1UL << 6)

/* USART_CR1 */
#define USART_CR1UE             (1UL << 13)
#define USART_CR1TE             (1UL << 3)
#define USART_CR1RE             (1UL << 2)
#define USART_CR1RXNEIE         (1UL << 5)

/* ========================================================================
 * DMA Registers
 * ======================================================================== */
#define DMA1_BASE       (AHBPERIPH_BASE + 0x0000UL)

typedef struct {
    volatile uint32_t CCR;
    volatile uint32_t CNDTR;
    volatile uint32_t CPAR;
    volatile uint32_t CMAR;
} DMA_Channel_TypeDef;

typedef struct {
    volatile uint32_t ISR;
    volatile uint32_t IFCR;
    DMA_Channel_TypeDef Channel[7];
} DMA_TypeDef;

#define DMA1            ((DMA_TypeDef *)DMA1_BASE)

/* DMA_CCR */
#define DMA_CCR_EN          (1UL << 0)
#define DMA_CCR_TCIE        (1UL << 1)
#define DMA_CCR_HTIE        (1UL << 2)
#define DMA_CCR_TEIE        (1UL << 3)
#define DMA_CCR_DIR         (1UL << 4)
#define DMA_CCR_CIRC        (1UL << 5)
#define DMA_CCR_PINC        (1UL << 6)
#define DMA_CCR_MINC        (1UL << 7)
#define DMA_CCR_PSIZE_8BIT  0x0UL
#define DMA_CCR_PSIZE_16BIT (1UL << 8)
#define DMA_CCR_PSIZE_32BIT (2UL << 8)
#define DMA_CCR_MSIZE_8BIT  0x0UL
#define DMA_CCR_MSIZE_16BIT (1UL << 10)
#define DMA_CCR_MSIZE_32BIT (2UL << 10)
#define DMA_CCR_MEM2MEM     (1UL << 14)

/* ========================================================================
 * Flash &滴heel
 * ======================================================================== */
#define SCB_BASE        0xE000ED00UL
typedef struct {
    volatile uint32_t CPUID;
    volatile uint32_t ICSR;
    volatile uint32_t VTOR;
    volatile uint32_t AIRCR;
    volatile uint32_t SCR;
    volatile uint32_t CCR;
    volatile uint8_t  SHP[12];
    volatile uint32_t SHCSR;
    volatile uint32_t CFSR;
    volatile uint32_t HFSR;
    volatile uint32_t MMFAR;
    volatile uint32_t BFAR;
} SCB_TypeDef;
#define SCB             ((SCB_TypeDef *)SCB_BASE)

/* ========================================================================
 * SysTick
 * ======================================================================== */
#define SysTick_BASE    0xE000E010UL
typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t LOAD;
    volatile uint32_t VAL;
    volatile uint32_t CALIB;
} SysTick_TypeDef;
#define SysTick         ((SysTick_TypeDef *)SysTick_BASE)
#define SysTick_CTRL_CLKSOURCE_AHB   (1UL << 2)
#define SysTick_CTRL_CLKSOURCE_AHB_8 (0UL << 2)
#define SysTick_CTRL_TICKINT          (1UL << 1)
#define SysTick_CTRL_ENABLE           (1UL << 0)

#endif /* STM32F1XX_H */
