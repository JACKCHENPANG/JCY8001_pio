/**
 * usart.h - USART2 driver for Modbus RTU
 *
 * JCY8001 PlatformIO Project
 *
 * 硬件: CP2102 USB转串口 (非RS422)
 * PA2=TX (AF PP), PA3=RX (Input floating)
 * 波特率: 115200
 */

#ifndef USART_H
#define USART_H

#include <stdint.h>

/**
 * Initialize USART2
 * @param baudrate 波特率 (e.g. 115200)
 * PA2=TX (AF PP 50MHz), PA3=RX (Input floating)
 */
void usart2_init(int baudrate);

/**
 * Send single byte
 */
void usart2_send(uint8_t byte);

/**
 * Send buffer
 */
void usart2_send_buf(const uint8_t *data, uint16_t len);

/**
 * Check if byte received, get it (non-blocking)
 * @return -1 if no data, else byte value
 */
int usart2_getc(void);

/**
 * USART2 ISR handler (call from interrupt)
 */
void USART2_IRQHandler(void);

#endif /* USART_H */
