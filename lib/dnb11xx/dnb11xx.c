/**
 * @file dnb11xx.c
 * @brief DNB11xx battery monitor IC driver
 *
 * Ported from: Drivers/Driver_DNB11xx.c (Keil RTX → HAL)
 * Note: Removed RTX threading (osThread, osMailQ, osMailCAlloc).
 *       Synchronous blocking API.
 */

#include <string.h>
#include "dnb11xx.h"
#include "spi.h"
#include "crc.h"

// TX/RX buffers (256 * 4 bytes = 1024 bytes per frame)
#define DNB11XX_BUF_LEN  (256 * 4)

static uint8_t  SendBuf[DNB11XX_BUF_LEN];
static uint8_t  RecvBuf[DNB11XX_BUF_LEN];
static DNB11xx_Result_t Results[DNB11XX_MAX_CHAIN];
static uint8_t  icCount = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void fill_frame(uint8_t *buf, uint8_t icIndex, uint8_t icCount,
                       const CMD_Union_t *cmd)
{
    uint32_t i;
    uint32_t base = icIndex * 4;

    // Pad 0x00 up to the IC's slot
    for (i = 0; i < base; i++) buf[i] = 0x00;

    // Header byte: 0x0F
    buf[base]     = 0x0F;
    buf[base + 1] = cmd->ucData[3];  // ID
    buf[base + 2] = cmd->ucData[2];  // CMD | Data[15:8]
    buf[base + 3] = cmd->ucData[1]; // Data[7:0] (CRC added below)

    // CRC over the 3 command bytes (CRC is 4 MSBs of the 4th byte)
    uint8_t crc = CRC4_TableMode(&buf[base], 3);
    buf[base + 3] = (buf[base + 3] & 0x0F) | (crc << 4);

    // Pad 0xFF for remaining ICs
    for (i = (icIndex + 1) * 4; i < (uint32_t)icCount * 4; i++) buf[i] = 0xFF;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void DNB11xx_Init(void)
{
    icCount = 0;
    memset(Results, 0, sizeof(Results));
    SPI1_Init();
}

uint8_t DNB11xx_GetICCount(void)
{
    return icCount;
}

const DNB11xx_Result_t *DNB11xx_GetResults(void)
{
    return Results;
}

uint8_t DNB11xx_Transfer(uint8_t *pSend, uint8_t *pRecv, uint8_t ics)
{
    if (!pSend || !pRecv || ics == 0 || ics > DNB11XX_MAX_CHAIN)
        return 0;

    SPI1_NSS_Low();
    // Full-duplex: TX + RX simultaneously
    if (SPI1_FullDuplex(pSend, pRecv, ics * 4, SPI_WORD_8BIT, SPI_BUF_U8) != SPI_OK) {
        SPI1_NSS_High();
        return 0;
    }
    SPI1_NSS_High();
    return 1;
}

uint8_t DNB11xx_TransferSingle(uint8_t slot, const uint8_t *pSend, uint8_t *pRecv, uint8_t ics)
{
    if (!pSend || !pRecv || slot >= ics || ics > DNB11XX_MAX_CHAIN)
        return 0;

    // Build full daisy-chain frame: command at slot position, 0x00 before, 0xFF after
    memset(SendBuf, 0xFF, DNB11XX_BUF_LEN);
    uint32_t slotBase = slot * 4;
    SendBuf[slotBase + 0] = pSend[0];
    SendBuf[slotBase + 1] = pSend[1];
    SendBuf[slotBase + 2] = pSend[2];
    SendBuf[slotBase + 3] = pSend[3];
    // Slots before 'slot' already zeroed; slots after already 0xFF

    memset(RecvBuf, 0xFF, DNB11XX_BUF_LEN);
    return DNB11xx_Transfer(SendBuf, RecvBuf, ics);
}

uint8_t DNB11xx_Enumerate(void)
{
    // Broadcast enumerate to all possible IC slots
    CMD_Union_t cmd = { .Field = { .CrcVal = 0, .Data = 0, .CMD = CMD_Type_Enumerate, .ID = 0xFF } };

    // Fix CRC
    uint8_t tmp[3] = { cmd.ucData[0], cmd.ucData[1], cmd.ucData[2] };
    cmd.Field.CrcVal = CRC4_TableMode(tmp, 3);

    uint32_t totalBytes = 0;
    for (uint8_t i = 0; i < DNB11XX_MAX_CHAIN; i++) {
        fill_frame(&SendBuf[totalBytes], i, 1, &cmd);
        totalBytes += 4;
    }

    memset(RecvBuf, 0xFF, DNB11XX_BUF_LEN);
    if (!DNB11xx_Transfer(SendBuf, RecvBuf, DNB11XX_MAX_CHAIN))
        return 0;

    // Parse responses: ID byte contains the IC's assigned ID
    uint8_t found = 0;
    for (uint8_t i = 0; i < DNB11XX_MAX_CHAIN; i++) {
        uint32_t base = i * 4;
        uint8_t idByte = RecvBuf[base + 1];
        if (idByte != 0xFF && idByte != 0x00) {
            Results[found].CMD.Field.ID = idByte;
            found++;
        }
    }
    icCount = found;
    return found;
}

uint8_t DNB11xx_Broadcast(const CMD_Union_t *cmd)
{
    if (!cmd || icCount == 0)
        return 0;

    // Build broadcast frames: one per IC position in the chain
    uint32_t totalBytes = 0;
    for (uint8_t i = 0; i < icCount; i++) {
        fill_frame(&SendBuf[totalBytes], i, icCount, cmd);
        totalBytes += 4;
    }

    memset(RecvBuf, 0xFF, DNB11XX_BUF_LEN);
    if (!DNB11xx_Transfer(SendBuf, RecvBuf, icCount))
        return 0;

    // Parse responses: slot position -> Results[slot]
    for (uint8_t slot = 0; slot < icCount; slot++) {
        uint32_t base = slot * 4;
        // RecvBuf[base+1] = ID field from IC at this slot
        uint8_t idFromIC = RecvBuf[base + 1];
        if (idFromIC != 0xFF && idFromIC != 0x00) {
            // Store the IC's ID for reference
            Results[slot].CMD.Field.ID = idFromIC;
        }
        // Store raw 4 bytes for parsing by caller
        Results[slot].GetData_MainVolt.ulData = 0;
        if (base + 3 < DNB11XX_BUF_LEN) {
            Results[slot].GetData_MainVolt.ucData[0] = RecvBuf[base];
            Results[slot].GetData_MainVolt.ucData[1] = RecvBuf[base + 1];
            Results[slot].GetData_MainVolt.ucData[2] = RecvBuf[base + 2];
            Results[slot].GetData_MainVolt.ucData[3] = RecvBuf[base + 3];
        }
    }
    return 1;
}

uint32_t DNB11xx_BuildSendBuf(uint8_t *ic_ids, uint8_t ics, const CMD_Union_t *cmd)
{
    (void)ic_ids;
    uint32_t totalBytes = 0;
    for (uint8_t i = 0; i < ics; i++) {
        fill_frame(&SendBuf[totalBytes], i, ics, cmd);
        totalBytes += 4;
    }
    return totalBytes;
}

Confirm_Union_t DNB11xx_GetConfirm(uint8_t *pRecvBuf, uint32_t headLen, uint8_t ic_id)
{
    Confirm_Union_t confirm = { .ulData = 0 };
    if (!pRecvBuf) return confirm;

    uint32_t base = (ic_id - 1) * 4;
    if (base + 3 >= DNB11XX_BUF_LEN) return confirm;

    confirm.ucData[0] = pRecvBuf[base];     // CRC
    confirm.ucData[1] = pRecvBuf[base + 1]; // Confirm[15:8]
    confirm.ucData[2] = pRecvBuf[base + 2]; // Confirm[7:0]
    confirm.ucData[3] = pRecvBuf[base + 3]; // ACK | ID
    return confirm;
}

/* ── Command builders ────────────────────────────────────────────────────── */

static uint8_t make_crc(const CMD_Union_t *cmd)
{
    uint8_t tmp[3] = { cmd->ucData[0], cmd->ucData[1], cmd->ucData[2] };
    return CRC4_TableMode(tmp, 3);
}

CMD_Union_t DNB11xx_Make_Init(uint8_t id, uint8_t nrOfICs, Init_AutoStb_t autoStb)
{
    CMD_Union_t cmd = { .ulData = 0 };
    cmd.Field.ID   = id;
    cmd.Field.CMD  = CMD_Type_Init;
    cmd.Field.Data = (nrOfICs & 0xFF) | ((autoStb & 0x03) << 8);
    cmd.Field.CrcVal  = make_crc(&cmd);
    return cmd;
}

CMD_Union_t DNB11xx_Make_GetStatus(uint8_t id, uint8_t statusType)
{
    CMD_Union_t cmd = { .ulData = 0 };
    cmd.Field.ID   = id;
    cmd.Field.CMD  = CMD_Type_GetStatus;
    cmd.Field.Data = statusType & 0x1F;
    cmd.Field.CrcVal  = make_crc(&cmd);
    return cmd;
}

CMD_Union_t DNB11xx_Make_GetData(uint8_t id, GetData_Type_t dtype)
{
    CMD_Union_t cmd = { .ulData = 0 };
    cmd.Field.ID   = id;
    cmd.Field.CMD  = CMD_Type_GetData;
    cmd.Field.Data = dtype & 0x3F;
    cmd.Field.CrcVal  = make_crc(&cmd);
    return cmd;
}

CMD_Union_t DNB11xx_Make_SetMode(uint8_t id, SetMode_Mode_t mode)
{
    CMD_Union_t cmd = { .ulData = 0 };
    cmd.Field.ID   = id;
    cmd.Field.CMD  = CMD_Type_SetMode;
    cmd.Field.Data = (mode & 0x0F);
    cmd.Field.CrcVal  = make_crc(&cmd);
    return cmd;
}
