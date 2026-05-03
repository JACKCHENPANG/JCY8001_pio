/**
 * @file eeprom.c
 * @brief AT24Cxx EEPROM driver (Software I2C)
 *
 * Hardware: PB6=SCL, PB7=SDA
 * Chip: AT24C128 (16K x 8, 64-byte page write, I2C @ 100/400kHz)
 *
 * Ported from: Drivers/Driver_24CXX.c (Keil RTX → HAL)
 * Note: Removed osDelay/osMutex calls — replaced with blocking waits + HAL_Delay.
 *       The original Driver_24CXX uses isBusy semaphore (protected by
 *       RTX osMutex) — here we use a static volatile flag with critical
 *       section protection instead.
 */

#include "eeprom.h"
#include "i2c.h"

// HAL_Delay is defined as __weak in stm32f1xx_hal.c (SysTick)
// Ensure SysTick_Handler + HAL_Init run before EEPROM_Write* is called.
extern void HAL_Delay(uint32_t ms);

// Device base I2C address (A0/A1/A2 grounded)
#define EEPROM_DEV_ADDR   0x50   // 1010 000 + 0(write)

// Critical section helpers
// Bare-metal single-threaded: is_busy flag alone is sufficient (no preemption)
#define ENTER_CRIT()   ((void)0)
#define EXIT_CRIT()    ((void)0)

// Internal state
static volatile uint8_t  eeprom_status = EEPROM_BUSY;
static volatile uint8_t  is_busy = 0;

/* ── Internal helpers ──────────────────────────────────────────────────────── */

/**
 * Wait until not busy (no mutex needed — caller already holds crit section)
 */
static inline void wait_not_busy(void)
{
    while (is_busy) { }
}

/**
 * Send word address bytes before data (AT24C128 uses 2-byte address)
 */
static void send_word_addr(uint16_t addr)
{
    I2C_Write((uint8_t)(addr >> 8));   // high byte
    I2C_Write((uint8_t)(addr & 0xFF)); // low byte
}

/**
 * Issue I2C START + send device write address
 * Returns 1 on ACK, 0 on NACK/timeout.
 */
static uint8_t start_write(uint16_t addr)
{
    I2C_Start();
    if (!I2C_Write(EEPROM_DEV_ADDR)) { // write bit
        I2C_Stop();
        return 0;
    }
    send_word_addr(addr);
    return 1;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void EEPROM_Init(void)
{
    I2C_Init();
    eeprom_status = EEPROM_Check();
}

uint8_t EEPROM_ReadByte(uint16_t addr)
{
    uint8_t data;

    // Atomic: protect is_busy flag
    ENTER_CRIT();
    wait_not_busy();
    is_busy = 1;
    EXIT_CRIT();

    I2C_Start();
    if (!I2C_Write(EEPROM_DEV_ADDR)) {
        I2C_Stop();
        is_busy = 0;
        return 0xFF;
    }
    send_word_addr(addr);

    I2C_Start();                          // repeated START
    I2C_Write(EEPROM_DEV_ADDR | 0x01);    // read bit
    data = I2C_Read(0);                   // NACK on last byte
    I2C_Stop();

    is_busy = 0;
    return data;
}

void EEPROM_WriteByte(uint16_t addr, uint8_t data)
{
    ENTER_CRIT();
    wait_not_busy();
    is_busy = 1;
    EXIT_CRIT();

    if (start_write(addr)) {
        I2C_Write(data);
        I2C_Stop();
        HAL_Delay(10);   // AT24C128 write cycle: 5ms max; use 10ms safety margin
    } else {
        I2C_Stop();
    }

    is_busy = 0;
}

/**
 * Write multiple bytes respecting page boundaries.
 * AT24C128 page size = 64 bytes.
 * If addr is not page-aligned, the partial first page is written,
 * then full pages, then the remaining tail.
 *
 * Returns: 0 on error, len on success.
 */
uint16_t EEPROM_WriteBlock(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    uint16_t written = 0;

    if (!buf || len == 0)
        return 0;

    while (written < len) {
        uint16_t page_remain = EEPROM_PAGE_SIZE - (addr % EEPROM_PAGE_SIZE);
        uint16_t chunk = (len - written <= page_remain) ? (len - written) : page_remain;

        ENTER_CRIT();
        wait_not_busy();
        is_busy = 1;
        EXIT_CRIT();

        if (!start_write(addr)) {
            I2C_Stop();
            is_busy = 0;
            return written;  // partial write
        }

        for (uint16_t i = 0; i < chunk; i++) {
            I2C_Write(buf[written + i]);
        }
        I2C_Stop();
        HAL_Delay(10);  // write cycle time
        is_busy = 0;

        written += chunk;
        addr    += chunk;
    }

    return written;
}

/**
 * Read multiple bytes (sequential read, no page limitation).
 *
 * Returns: 0 on error, len on success.
 */
uint16_t EEPROM_ReadBlock(uint16_t addr, uint8_t *buf, uint16_t len)
{
    if (!buf || len == 0)
        return 0;

    ENTER_CRIT();
    wait_not_busy();
    is_busy = 1;
    EXIT_CRIT();

    I2C_Start();
    if (!I2C_Write(EEPROM_DEV_ADDR)) {
        I2C_Stop();
        is_busy = 0;
        return 0;
    }
    send_word_addr(addr);

    I2C_Start();
    I2C_Write(EEPROM_DEV_ADDR | 0x01);  // read bit

    for (uint16_t i = 0; i < len - 1; i++) {
        buf[i] = I2C_Read(1);  // ACK each byte except last
    }
    buf[len - 1] = I2C_Read(0);  // NACK last byte
    I2C_Stop();

    is_busy = 0;
    return len;
}

/**
 * Self-test: write 0x55/0xAA pattern to last 2 addresses.
 * Returns:
 *   0 = OK (pattern matched)
 *   1 = write/read mismatch on last addr
 *   2 = write/read mismatch on second-last addr
 *   3 = timeout
 */
uint8_t EEPROM_Check(void)
{
    uint16_t last_addr = EEPROM_TOTAL_SIZE - 1;
    uint8_t  val;

    // Try to read existing 0x55
    for (uint8_t i = 0; i < 10; i++) {
        val = EEPROM_ReadByte(last_addr);
        if (val == 0x55)
            return EEPROM_OK;
    }

    // Write 0x55 to last_addr
    EEPROM_WriteByte(last_addr, 0x55);

    // Try to read it back
    for (uint8_t i = 0; i < 10; i++) {
        val = EEPROM_ReadByte(last_addr);
        if (val == 0x55)
            return EEPROM_OK;
    }

    return EEPROM_ERR;
}

/* ── Query ───────────────────────────────────────────────────────────────── */

uint8_t  EEPROM_GetStatus(void)     { return eeprom_status; }
uint16_t EEPROM_GetTotalSize(void) { return EEPROM_TOTAL_SIZE; }
uint16_t EEPROM_GetPageSize(void)  { return EEPROM_PAGE_SIZE; }
