/**
 * @file dnb11xx.h
 * @brief DNB11xx battery monitor IC driver
 *
 * Communication: SPI1 (polling/DMA)
 * Protocol: Custom 32-bit frame with 4-bit CRC
 * Max chain: 9 ICs in daisy chain
 *
 * Ported from: Drivers/Driver_DNB11xx.c (Keil RTX → HAL)
 * Note: RTX osThread/osMailQ removed. Synchronous API with
 *       blocking waits. Thread creation removed.
 */

#ifndef _DNB11XX_HAL_H_
#define _DNB11XX_HAL_H_

#include <stdint.h>
#include <stdbool.h>

// ── Command types ──────────────────────────────────────────────────────────

typedef enum {
    CMD_Type_Enumerate      = 0x00,
    CMD_Type_Init           = 0x01,
    CMD_Type_SetMTPLockKey  = 0x02,
    CMD_Type_SetThVolt      = 0x03,
    CMD_Type_SetThTemp      = 0x04,
    CMD_Type_SetZMCurr      = 0x06,
    CMD_Type_SetZMFreq      = 0x07,
    CMD_Type_SetBalCurr     = 0x08,
    CMD_Type_SetBalVolt     = 0x09,
    CMD_Type_SetSrvReqMask  = 0x0a,
    CMD_Type_SetMode        = 0x0b,
    CMD_Type_SetForcedErr   = 0x0c,
    CMD_Type_GetStatus      = 0x0d,
    CMD_Type_GetData        = 0x0e,
    CMD_Type_SetRegBank     = 0x0f,
} CMD_Type_t;

// Command frame (32-bit, LSB first on wire)
typedef union {
    struct {
        uint32_t    CrcVal   : 4;
        uint32_t    Data     : 16;
        uint32_t    CMD      : 4;
        uint32_t    ID       : 8;
    } Field;
    uint32_t    ulData;
    uint8_t     ucData[4];
} CMD_Union_t;

// Confirm frame (32-bit response)
typedef union {
    struct {
        uint32_t    CrcVal   : 4;
        uint32_t    Confirm  : 16;
        uint32_t    ACK      : 4;
        uint32_t    ID       : 8;
    } Field;
    uint32_t    ulData;
    uint8_t     ucData[4];
} Confirm_Union_t;

// ── Result data ─────────────────────────────────────────────────────────────

// AutoStandby setting
typedef enum {
    Init_AutoStb_NoAutoStb  = 0,
    Init_AutoStb_NoAutoStb1  = 1,
    Init_AutoStb_AutoStb1    = 2,
    Init_AutoStb_AutoStb2    = 3,
} Init_AutoStb_t;

// Operating mode
typedef enum {
    SetMode_Mode_Sleep   = 1,
    SetMode_Mode_Standby = 3,
    SetMode_Mode_Normal  = 4,
    SetMode_Mode_Self    = 6,
} SetMode_Mode_t;

// FSM state
typedef enum {
    FSMStatus_Sleep    = 0x01,
    FSMStatus_Standby  = 0x03,
    FSMStatus_Normal   = 0x04,
    FSMStatus_SelfTest = 0x06,
    FSMStatus_Safe     = 0x08,
    FSMStatus_SPI      = 0x0C,
} FSMStatus_t;

// Data type for GetData command
typedef enum {
    GetData_Type_MainVolt    = 0x00,
    GetData_Type_GuardVolt   = 0x01,
    GetData_Type_MainCellTemp= 0x02,
    GetData_Type_GuardCellTemp=0x03,
    GetData_Type_MainDieTemp = 0x04,
    GetData_Type_GuardDieTemp= 0x05,
    GetData_Type_VZM         = 0x06,
    GetData_Type_Zreal       = 0x07,
    GetData_Type_Zimag       = 0x08,
    GetData_Type_UniqueID1   = 0x09,
    GetData_Type_UniqueID2   = 0x0a,
    GetData_Type_UniqueID3   = 0x0b,
    GetData_Type_UniqueID4   = 0x0c,
    GetData_Type_HwChkMTP    = 0x0d,
    GetData_Type_HwChkSet    = 0x0e,
    GetData_Type_ProductVer  = 0x0f,
} GetData_Type_t;

// Result struct (one entry per IC in chain)
#define DNB11XX_MAX_CHAIN  9

typedef struct {
    // SET command confirmation
    Confirm_Union_t   Confirm;
    CMD_Union_t        CMD;

    // GET STATUS results
    CMD_Union_t        GetStatus_CheckID;
    CMD_Union_t        GetStatus_GeneralStatus;
    CMD_Union_t        GetStatus_PLLStatus;
    CMD_Union_t        GetStatus_BALStatus;
    CMD_Union_t        GetStatus_SrvReqData;
    CMD_Union_t        GetStatus_VoltDiagnostics;
    CMD_Union_t        GetStatus_TempDiagnostics;
    CMD_Union_t        GetStatus_CurrDiagnostics;
    CMD_Union_t        GetStatus_ZMDiagnostics;
    CMD_Union_t        GetStatus_ICDiagnostics;

    // GET DATA results
    CMD_Union_t        GetData_MainVolt;
    CMD_Union_t        GetData_GuardVolt;
    CMD_Union_t        GetData_MainCellTemp;
    CMD_Union_t        GetData_GuardCellTemp;
    CMD_Union_t        GetData_MainDieTemp;
    CMD_Union_t        GetData_GuardDieTemp;
    CMD_Union_t        GetData_VZM;
    CMD_Union_t        GetData_Zreal;
    CMD_Union_t        GetData_Zimag;
    CMD_Union_t        GetData_UniqueID1;
    CMD_Union_t        GetData_UniqueID2;
    CMD_Union_t        GetData_UniqueID3;
    CMD_Union_t        GetData_UniqueID4;
    CMD_Union_t        GetData_HwChksumMTP;
    CMD_Union_t        GetData_HwChksumSet;
    CMD_Union_t        GetData_ProductVer;
} DNB11xx_Result_t;

// ── Public API ─────────────────────────────────────────────────────────────

/**
 * Initialize DNB11xx driver + SPI1.
 * Call once at startup.
 */
void DNB11xx_Init(void);

/**
 * Probe the daisy chain and count live ICs.
 * Stores result in DNB11xx_Results[0..N-1].
 * Returns: number of ICs found.
 */
uint8_t DNB11xx_Enumerate(void);

/**
 * Send a raw command frame to the daisy chain and read responses.
 * @param pSend  TX buffer (4 bytes per IC)
 * @param pRecv  RX buffer (4 bytes per IC)
 * @param ics    Number of ICs in chain
 * @return 0 on error, 1 on success
 */
uint8_t DNB11xx_Transfer(uint8_t *pSend, uint8_t *pRecv, uint8_t ics);

/**
 * Build a send buffer for multi-IC command.
 * Returns the total number of bytes written.
 */
uint32_t DNB11xx_BuildSendBuf(uint8_t *ic_ids, uint8_t ics,
                               const CMD_Union_t *cmd);

/**
 * Read back confirm responses after a SET command.
 */
Confirm_Union_t DNB11xx_GetConfirm(uint8_t *pRecvBuf, uint32_t headLen, uint8_t ic_id);

/**
 * Transfer 4 bytes to a specific IC slot and read its response.
 * @param slot   IC slot position (0-based)
 * @param pSend  4-byte command frame
 * @param pRecv  4-byte response buffer
 * @param ics    Total ICs in chain (for padding)
 * @return 0 on error, 1 on success
 */
uint8_t DNB11xx_TransferSingle(uint8_t slot, const uint8_t *pSend, uint8_t *pRecv, uint8_t ics);

/**
 * Broadcast a command to all ICs in the daisy chain (ID = 0xFF).
 * Sends the command for each IC position and reads all responses.
 * Returns: 0 on error, 1 on success.
 */
uint8_t DNB11xx_Broadcast(const CMD_Union_t *cmd);

/**
 * Get the number of ICs in the chain.
 */
uint8_t DNB11xx_GetICCount(void);

/**
 * Access raw result array.
 */
const DNB11xx_Result_t *DNB11xx_GetResults(void);

// ── Command builders ───────────────────────────────────────────────────────

CMD_Union_t DNB11xx_Make_Init(uint8_t id, uint8_t nrOfICs,
                                Init_AutoStb_t autoStb);
CMD_Union_t DNB11xx_Make_GetStatus(uint8_t id, uint8_t statusType);
CMD_Union_t DNB11xx_Make_GetData(uint8_t id, GetData_Type_t dtype);
CMD_Union_t DNB11xx_Make_SetMode(uint8_t id, SetMode_Mode_t mode);

#endif // _DNB11XX_HAL_H_
