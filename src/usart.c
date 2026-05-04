/*
 * usart.c - USART2 driver for Modbus RTU
 *
 * JCY8001 PlatformIO Project
 *
 * 硬件: CP2102 USB转串口 (非RS422)
 * PA2=TX (AF PP), PA3=RX (Input floating)
 * 波特率: 115200
 */

#include "../inc/usart.h"
#include "../inc/stm32f1xx.h"

// USART标志
#define USART_SR_TXE   (1UL << 7)
#define USART_SR_RXNE  (1UL << 5)

// 接收缓冲
static uint8_t rx_buf[256];
static uint8_t rx_head = 0;
static uint8_t rx_tail = 0;

/*
 * Initialize USART2
 * PA2=TX (AF PP 50MHz), PA3=RX (Input floating)
 * 8N1, 115200 baud
 */
void usart2_init(int baudrate) {
    (void)baudrate;  /* 暂时忽略参数，使用固定115200 */

    /* 1. GPIOA 时钟 */
    RCC->APB2ENR |= (1UL << 2);  /* IOPAEN */

    /* 2. USART2 时钟 (APB1) */
    RCC->APB1ENR |= (1UL << 17);  /* USART2EN */

    /* 3. PA2=TX(AF PP 50MHz), PA3=RX(input floating) */
    uint32_t crl = GPIOA->CRL;
    crl &= ~(0xFFUL << 8);   /* 清除 PA2 PA3 */
    crl |=  (0xBUL << 8);    /* PA2: AF PP 50MHz */
    crl |=  (0x4UL << 12);   /* PA3: Input floating */
    GPIOA->CRL = crl;

    /* 4. BRR = 0x138 -> 36MHz / 115200 (main.c: HSE×9=72MHz, APB1=/2 → PCLK1=36MHz)
     *    BRR = 36000000 / 115200 = 312.5 → 0x138
     *    36MHz / 312 = 115385 baud (+0.16% error, within ±2% tolerance for Modbus) */
    USART2->BRR = 0x138;

    /* 5. CR1: UE + TE + RE + RXNEIE */
    USART2->CR1 = (1UL << 13) | (1UL << 3) | (1UL << 2) | (1UL << 5);
}

void usart2_send(uint8_t byte) {
    while ((USART2->SR & USART_SR_TXE) == 0);
    USART2->DR = byte;
}

void usart2_send_buf(const uint8_t *data, uint16_t len) {
    while (len--) {
        while ((USART2->SR & USART_SR_TXE) == 0);
        USART2->DR = *data++;
    }
}

int usart2_getc(void) {
    if (rx_head == rx_tail) {
        return -1;  /* 无数据 */
    }
    uint8_t c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % sizeof(rx_buf);
    return c;
}

/*
 * USART2 中断处理
 */
void USART2_IRQHandler(void) {
    if (USART2->SR & USART_SR_RXNE) {
        uint8_t byte = (uint8_t)USART2->DR;
        uint8_t next = (rx_head + 1) % sizeof(rx_buf);
        if (next != rx_tail) {
            rx_buf[rx_head] = byte;
            rx_head = next;
        }
    }
}
