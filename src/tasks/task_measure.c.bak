/**
 * @file task_measure.c
 * @brief Measurement task
 *
 * Ported from: Threads/MeasThread.c (Keil RTX -> FreeRTOS)
 *
 * Measurement cycle (every 500ms):
 *   1. Broadcast GetStatus(SrvReqData) + GetStatus(GeneralStatus)
 *   2. Broadcast GetData(MainVolt) + GetData(MainDieTemp)
 *   3. Check BalZMDone flag → if set, read Zreal/Zimag/VZM
 *
 * Results are stored in shared Modbus register area for host readout.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "task_measure.h"
#include "dnb11xx.h"
#include <string.h>

/* ── Shared measurement data (updated by measure task, read by Modbus) ─────── */

/* Per-IC measurement results */
typedef struct {
    uint16_t  voltage_mv;      /* Battery voltage in mV */
    int16_t   temperature_decidegC; /* Temperature in 0.1°C units */
    uint16_t  zm_volt;          /* ZM voltage (raw 12-bit ADC) */
    uint16_t  zm_zreal;         /* Zreal mantissa | (exponent << 12) */
    uint16_t  zm_zimag;         /* Zimag mantissa | (exponent << 12) */
    uint8_t   bal_zm_done;       /* ZM measurement complete flag */
    uint8_t   fsm_status;       /* IC FSM state */
    uint8_t   valid;            /* Data valid flag */
} MeasData_t;

static MeasData_t s_meas[DNB11XX_MAX_CHAIN];
static volatile uint8_t s_busy = 0;
static uint8_t s_ic_count = 0;

/* 500ms tick counter */
static volatile uint32_t s_tick_500ms = 0;

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/* Parse voltage from GetData response
 * Formula: (adc / 16383.0) * 4800 + 1200  [mV]
 * 14-bit ADC, 4.8V FS range, 1.2V offset */
static uint16_t parse_voltage(uint32_t raw)
{
    uint32_t adc = raw & 0x3FFF; /* 14-bit */
    uint32_t mv = (adc * 4800UL) / 16383UL + 1200UL;
    return (uint16_t)(mv > 65535 ? 65535 : mv);
}

/* Parse die temperature from GetData response
 * 12-bit signed, 0.0625°C per LSB, sign-extended */
static int16_t parse_temperature(uint32_t raw)
{
    uint32_t val = raw & 0xFFF;  /* 12-bit */
    /* Sign extend */
    if (val & 0x800)
        val |= 0xF000;
    int16_t temp = (int16_t)val;
    return (int16_t)(temp * 625 / 10); /* Convert to deci-degrees C */
}

/* Check BalZMDone flag from SrvReqData response word */
static uint8_t check_bal_zm_done(uint32_t raw)
{
    /* SrvReqData bits: [ACK:4][ID:8][IntErr:1][ClockErr:1][CmdErr:1][Reserved2:1]
     *                  [InvalidLockKey:1][Reserved:1][ZMADCErr:1][BalZMDone:1]
     *                  [CurrErr:1][LDOOoR:1][TempADCErr:1][CellTempErr:1][VMADCErr:1]
     * BalZMDone is bit 5 (zero-indexed from LSB) */
    return (raw & (1U << 5)) ? 1 : 0;
}

/* ── Measurement command helpers ─────────────────────────────────────────── */

/* Build and send a broadcast GetData command, store result in result_idx */
static uint8_t broadcast_get_data(GetData_Type_t dtype)
{
    CMD_Union_t cmd = DNB11xx_Make_GetData(0xFF, dtype);
    if (!DNB11xx_Broadcast(&cmd))
        return 0;

    const DNB11xx_Result_t *res = DNB11xx_GetResults();
    for (uint8_t slot = 0; slot < s_ic_count; slot++) {
        uint32_t raw = res[slot].GetData_MainVolt.ulData;
        switch (dtype) {
            case GetData_Type_MainVolt:
                s_meas[slot].voltage_mv = parse_voltage(raw);
                break;
            case GetData_Type_MainDieTemp:
                s_meas[slot].temperature_decidegC = parse_temperature(raw);
                break;
            case GetData_Type_VZM:
                /* Lower 12 bits are ZM voltage */
                s_meas[slot].zm_volt = (uint16_t)(raw & 0xFFF);
                break;
            case GetData_Type_Zreal:
                /* Mantissa = bits[3:14], Exponent = bits[16:19] */
                s_meas[slot].zm_zreal = (uint16_t)((raw >> 3) & 0xFFFF);
                break;
            case GetData_Type_Zimag:
                s_meas[slot].zm_zimag = (uint16_t)((raw >> 3) & 0xFFFF);
                break;
            default:
                break;
        }
    }
    return 1;
}

/* Build and send a broadcast GetStatus command, store parsed flags */
static uint8_t broadcast_get_status(uint8_t status_type)
{
    CMD_Union_t cmd = DNB11xx_Make_GetStatus(0xFF, status_type);
    if (!DNB11xx_Broadcast(&cmd))
        return 0;

    const DNB11xx_Result_t *res = DNB11xx_GetResults();
    for (uint8_t slot = 0; slot < s_ic_count; slot++) {
        uint32_t raw = res[slot].GetData_MainVolt.ulData;
        if (status_type == 0x12) { /* SrvReqData */
            s_meas[slot].bal_zm_done = check_bal_zm_done(raw);
        }
        /* GeneralStatus: FSM state is bits[20:23] of GetStatus response */
        /* We store the raw for now */
        (void)raw;
    }
    return 1;
}

/* ── Main measurement task ────────────────────────────────────────────────── */

void vTaskMeasure(void *pvParameters)
{
    (void)pvParameters;

    memset(s_meas, 0, sizeof(s_meas));

    /* Wait for main() to finish DNB11xx_Init + Enumerate.
     * main() sets dnb_ic_count after enumeration. */
    vTaskDelay(pdMS_TO_TICKS(200));
    extern uint8_t dnb_ic_count;
    s_ic_count = dnb_ic_count;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        s_tick_500ms++;

        uint8_t ic = s_ic_count;
        if (ic == 0 || s_busy)
            continue;

        s_busy = 1;

        /* Step 1: Broadcast GetStatus (SrvReqData + GeneralStatus) */
        broadcast_get_status(0x12);  /* SrvReqData */
        broadcast_get_status(0x02);  /* GeneralStatus */

        /* Step 2: Broadcast GetData (MainVolt + MainDieTemp) */
        broadcast_get_data(GetData_Type_MainVolt);
        broadcast_get_data(GetData_Type_MainDieTemp);

        /* Step 3: If BalZMDone, read ZM data for each specific IC */
        for (uint8_t slot = 0; slot < ic && slot < DNB11XX_MAX_CHAIN; slot++) {
            if (s_meas[slot].bal_zm_done) {
                s_meas[slot].bal_zm_done = 0; /* Clear flag */
                /* Send Zreal, Zimag, VZM commands to this specific IC.
                 * Use DNB11xx_TransferSingle to build a proper daisy-chain frame. */
                CMD_Union_t cmd_zr = DNB11xx_Make_GetData(slot + 1, GetData_Type_Zreal);
                CMD_Union_t cmd_zi = DNB11xx_Make_GetData(slot + 1, GetData_Type_Zimag);
                CMD_Union_t cmd_vz = DNB11xx_Make_GetData(slot + 1, GetData_Type_VZM);
                uint8_t recv[4];

                if (DNB11xx_TransferSingle(slot, cmd_zr.ucData, recv, ic)) {
                    /* Parse Zreal: bits[3:14]=Mantissa, bits[16:19]=Exponent */
                    uint32_t raw = ((uint32_t)recv[3] << 24) | ((uint32_t)recv[2] << 16) |
                                   ((uint32_t)recv[1] << 8) | recv[0];
                    s_meas[slot].zm_zreal = (uint16_t)((raw >> 3) & 0xFFFF);
                }
                if (DNB11xx_TransferSingle(slot, cmd_zi.ucData, recv, ic)) {
                    uint32_t raw = ((uint32_t)recv[3] << 24) | ((uint32_t)recv[2] << 16) |
                                   ((uint32_t)recv[1] << 8) | recv[0];
                    s_meas[slot].zm_zimag = (uint16_t)((raw >> 3) & 0xFFFF);
                }
                if (DNB11xx_TransferSingle(slot, cmd_vz.ucData, recv, ic)) {
                    uint32_t raw = ((uint32_t)recv[3] << 24) | ((uint32_t)recv[2] << 16) |
                                   ((uint32_t)recv[1] << 8) | recv[0];
                    s_meas[slot].zm_volt = (uint16_t)(raw & 0xFFF);
                }
                s_meas[slot].valid = 1;
            }
        }

        s_busy = 0;
    }
}

/* ── API for other tasks / Modbus ─────────────────────────────────────────── */

uint8_t Measure_GetCount(void)
{
    return DNB11xx_GetICCount();
}

uint16_t Measure_GetVoltage(uint8_t ic_idx)
{
    if (ic_idx >= DNB11XX_MAX_CHAIN || s_busy)
        return 0;
    return s_meas[ic_idx].voltage_mv;
}

int16_t Measure_GetTemperature(uint8_t ic_idx)
{
    if (ic_idx >= DNB11XX_MAX_CHAIN || s_busy)
        return 0;
    return s_meas[ic_idx].temperature_decidegC;
}

uint16_t Measure_GetZreal(uint8_t ic_idx)
{
    if (ic_idx >= DNB11XX_MAX_CHAIN || s_busy)
        return 0;
    return s_meas[ic_idx].zm_zreal;
}

uint16_t Measure_GetZimag(uint8_t ic_idx)
{
    if (ic_idx >= DNB11XX_MAX_CHAIN || s_busy)
        return 0;
    return s_meas[ic_idx].zm_zimag;
}

uint16_t Measure_GetZMV(uint8_t ic_idx)
{
    if (ic_idx >= DNB11XX_MAX_CHAIN || s_busy)
        return 0;
    return s_meas[ic_idx].zm_volt;
}

uint8_t Measure_IsValid(uint8_t ic_idx)
{
    if (ic_idx >= DNB11XX_MAX_CHAIN)
        return 0;
    return s_meas[ic_idx].valid;
}

uint32_t Measure_GetTick500ms(void)
{
    return s_tick_500ms;
}
