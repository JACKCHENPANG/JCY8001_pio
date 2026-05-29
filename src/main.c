/**
 * JCY8001 Modbus RTU Firmware v2.3 (PlatformIO bare-metal)
 * 
 * USART2 (PA2/PA3) 连接 CP2102, 115200 8N1
 * SPI1 (PA5/PA6/PA7, PB2=NSS) DNB1101
 * 支持: FC01, FC03, FC04, FC06 + DNB1101 GetData
 */
#include <stdint.h>
#include "stm32f1xx.h"
#include "dnb_zm.h"   // 阻抗换算 + 校准表 (lib/dnb_zm)

/* ── JCY8001 Registers ──────────────────────────────────────────────────── */

volatile uint16_t jcy_ch_count;
volatile uint16_t jcy_version;
volatile uint16_t jcy_temp;
volatile uint16_t jcy_voltage;
volatile uint16_t jcy_status;
volatile uint16_t jcy_zm_freq;
volatile uint16_t jcy_zm_avg;
volatile uint16_t jcy_fw_version;
volatile uint16_t jcy_git_rev;
volatile uint16_t jcy_build_date;
volatile uint16_t jcy_dnb_debug;     // DNB枚举调试：版本字节
volatile uint16_t jcy_dnb_volt_raw;  // DNB GetData MainVolt 原始值
volatile uint16_t jcy_dnb_temp_raw;  // DNB GetData MainDieTemp 原始值
volatile uint16_t jcy_dnb_zmv_raw;   // DNB GetData VZM 原始值
volatile uint16_t jcy_dnb_rx0;       // SPI RX raw bytes [0..1]
volatile uint16_t jcy_dnb_rx1;       // SPI RX raw bytes [2..3]
volatile uint16_t jcy_dnb_rx2;       // SPI RX raw bytes [4..5]
volatile uint16_t jcy_dnb_rx3;       // SPI RX raw bytes [6..7]
volatile uint16_t jcy_dnb_gs_hi;     // GeneralStatus high word
volatile uint16_t jcy_dnb_gs_lo;     // GeneralStatus low word
volatile uint16_t jcy_dnb_ss_hi;     // SrvReqStatus high word
volatile uint16_t jcy_dnb_ss_lo;     // SrvReqStatus low word
volatile uint16_t jcy_dnb_phase_dbg; // measurement phase / state
// ── 阻抗测量 (ZM/EIS) ──
volatile uint16_t jcy_zm_re;         // Zreal 原始 (ZMantissa|ZExponent<<12), 调试
volatile uint16_t jcy_zm_im;         // Zimag 原始, 调试
volatile uint16_t jcy_zm_vzm;        // VZM 原始
volatile int64_t  jcy_zm_re64;       // 换算后实部 (×100000, 主机 /100000=μΩ), 0x3000 64位
volatile int64_t  jcy_zm_im64;       // 换算后虚部, 0x3080 64位
volatile uint16_t jcy_zm_freq_set;   // 阻抗测量频率设置 (FRQMantissa|FRQExp<<8|LFNS<<12)
volatile uint16_t jcy_zm_done;       // 最近一次 ZM 是否完成 (BalZMDone)
volatile uint8_t  zm_start_req;      // FC05 线圈 0x0000=ON 触发的启动请求
// ── 参数寄存器 (主机经 FC03/06/10 读写) + 均衡 ──
volatile uint16_t jcy_samp_res;      // 0x40C0 采样电阻 0~3 (0=20R,1=10R,2=6.67R,3=5R)
volatile uint16_t jcy_zm_gain;       // 0x4280 ZM 增益 (1/4/16)
volatile uint16_t jcy_bal_volt;      // 0x4100 均衡电压 0~255 (mV=1200+值*18.8)
volatile uint16_t jcy_bal_time;      // 0x4140 均衡时间 0~255 (1LSB=134s)
volatile uint16_t jcy_bal_pwm;       // 0x4180 PWM 占空比 0~14
volatile uint16_t jcy_bal_mode;      // 0x0080 均衡模式 0=时间 1=电压
volatile uint8_t  bal_start_req;     // FC05/0F 线圈 0x0040 触发均衡启动/停止
volatile uint16_t jcy_zm_mode;       // 0x4300 ZM 测量模式: 0=普通, 1=低阻低频(强制 LFNS+增益16)

#define DNB_BUFLEN  512U
static uint8_t dnb_tx[DNB_BUFLEN];
static uint8_t dnb_rx[DNB_BUFLEN];

static void init_registers(void) {
    jcy_ch_count   = 1;
    jcy_version    = 0x0002;
    jcy_temp       = 0;
    jcy_voltage    = 0;
    jcy_status     = 0x0003;
    jcy_zm_freq    = 40;
    jcy_zm_avg     = 10;
    jcy_fw_version = 0x0200;
    jcy_git_rev    = 0x0001;
    jcy_build_date = 0x0504;
    jcy_dnb_debug  = 0;
    jcy_dnb_volt_raw = 0;
    jcy_dnb_temp_raw = 0;
    jcy_dnb_zmv_raw  = 0;
    jcy_zm_re = 0; jcy_zm_im = 0; jcy_zm_vzm = 0; jcy_zm_done = 0;
    jcy_zm_re64 = 0; jcy_zm_im64 = 0;
    jcy_zm_freq_set = 0x0746;   // 默认 66.757Hz: FRQMantissa=70, FRQExp=7 (校准表真实项, idx=413)
    zm_start_req = 0;
    jcy_samp_res = 0; jcy_zm_gain = 1; jcy_bal_volt = 133; jcy_bal_time = 0;
    jcy_bal_pwm = 0; jcy_bal_mode = 0; bal_start_req = 0;
    jcy_zm_mode = 0;
}

static uint16_t get_reg(uint16_t addr) {
    switch (addr) {
        case 0x3E00: return jcy_ch_count;
        case 0x3E01: return jcy_version;
        case 0x3E02: return jcy_fw_version;
        case 0x3E03: return jcy_git_rev;
        case 0x3E04: return jcy_build_date;
        case 0x3E20: return jcy_dnb_debug;
        case 0x3E21: return jcy_dnb_rx0;
        case 0x3E22: return jcy_dnb_rx1;
        case 0x3E23: return jcy_dnb_rx2;
        case 0x3E24: return jcy_dnb_rx3;
        case 0x3E25: return jcy_dnb_volt_raw;
        case 0x3E26: return jcy_dnb_temp_raw;
        case 0x3E27: return jcy_dnb_zmv_raw;
        case 0x3E28: return jcy_dnb_gs_hi;
        case 0x3E29: return jcy_dnb_gs_lo;
        case 0x3E2A: return jcy_dnb_ss_hi;
        case 0x3E2B: return jcy_dnb_ss_lo;
        case 0x3E2C: return jcy_dnb_phase_dbg;
        case 0x3300: return jcy_temp;
        case 0x3340: return jcy_voltage;
        case 0x3380: return jcy_status;
        // 阻抗实部 RE 0x3000~0x3003 / 虚部 IM 0x3080~0x3083: 64位有符号大端, 主机 /100000=μΩ。
        case 0x3000: return (uint16_t)(jcy_zm_re64 >> 48);
        case 0x3001: return (uint16_t)(jcy_zm_re64 >> 32);
        case 0x3002: return (uint16_t)(jcy_zm_re64 >> 16);
        case 0x3003: return (uint16_t)(jcy_zm_re64);
        case 0x3080: return (uint16_t)(jcy_zm_im64 >> 48);
        case 0x3081: return (uint16_t)(jcy_zm_im64 >> 32);
        case 0x3082: return (uint16_t)(jcy_zm_im64 >> 16);
        case 0x3083: return (uint16_t)(jcy_zm_im64);
        case 0x3200: return jcy_zm_vzm;
        case 0x3E2D: return jcy_zm_done;
        case 0x3E2E: return jcy_zm_re;   // 调试: 原始 M/E
        case 0x3E2F: return jcy_zm_im;
        case 0x4000: return jcy_zm_freq;
        case 0x4040: return jcy_zm_avg;
        case 0x40C0: return jcy_samp_res;
        case 0x4100: return jcy_bal_volt;
        case 0x4140: return jcy_bal_time;
        case 0x4180: return jcy_bal_pwm;
        case 0x4200: return jcy_zm_freq_set;
        case 0x4280: return jcy_zm_gain;
        case 0x4300: return jcy_zm_mode;          // ZM 测量模式 0=普通 1=低阻低频
        default:     return 0x0000;
    }
}

static void set_reg(uint16_t addr, uint16_t val) {
    switch (addr) {
        case 0x3E00: jcy_ch_count = val; break;
        case 0x3E01: jcy_version = val; break;
        case 0x3E02: jcy_fw_version = val; break;
        case 0x3E03: jcy_git_rev = val; break;
        case 0x3E04: jcy_build_date = val; break;
        case 0x3300: jcy_temp = val; break;
        case 0x3340: jcy_voltage = val; break;
        case 0x3380: jcy_status = val; break;
        case 0x4000: jcy_zm_freq = val; break;
        case 0x4040: jcy_zm_avg = val; break;
        case 0x40C0: jcy_samp_res = val & 0x03; break;       // 采样电阻 0~3
        case 0x4100: jcy_bal_volt = val & 0xFF; break;       // 均衡电压 0~255
        case 0x4140: jcy_bal_time = val & 0xFF; break;       // 均衡时间 0~255
        case 0x4180: jcy_bal_pwm = val & 0x0F; break;        // PWM 0~14
        case 0x4200: jcy_zm_freq_set = val; break;           // 阻抗测量频率 (FRQMantissa|FRQExp<<8|LFNS<<12)
        case 0x4280: jcy_zm_gain = val; break;               // ZM 增益 1/4/16
        // ── 群发地址 (写全部通道; 本单通道板=写本通道) ──
        case 0x4F01: jcy_zm_avg   = val; break;              // 群发 ZM 平均次数
        case 0x4F03: jcy_samp_res = val & 0x03; break;       // 群发 采样电阻
        case 0x4F04: jcy_bal_volt = val & 0xFF; break;       // 群发 均衡电压
        case 0x4F05: jcy_bal_time = val & 0xFF; break;       // 群发 均衡时间
        case 0x4F06: jcy_bal_pwm  = val & 0x0F; break;       // 群发 PWM
        case 0x4F07: jcy_zm_freq_set = val; break;           // 群发 测量频率 (低字)
        case 0x4F09: jcy_zm_gain  = val; break;              // 群发 ZM 增益
        case 0x4300: jcy_zm_mode  = val; break;              // ZM 测量模式 0=普通 1=低阻低频
        case 0x4F0A: jcy_zm_mode  = val; break;              // 群发 ZM 测量模式
    }
}

/* ── USART2 ─────────────────────────────────────────────────────────────── */

static void usart2_send(uint8_t c) {
    while (!(USART2->SR & USART_SR_TXE));
    USART2->DR = c;
}

static void usart2_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    GPIOA->CRL = (GPIOA->CRL & 0xFFFF0000) | 0x00004B04;
    USART2->BRR = 0x0045;
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

/* ── SPI1 (DNB1101) ─────────────────────────────────────────────────────── */

static uint8_t dnb_ic_count = 0;

static void spi1_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN | RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;
    GPIOA->CRL = (GPIOA->CRL & 0x0000FFFF) | 0xB4B00000;
    GPIOB->CRL = (GPIOB->CRL & 0xFFFFF0FF) | 0x00000300;
    GPIOB->BSRR = GPIO_BSRR_BS2;
    SPI1->CR1 = 0x0354;
}

static void spi1_nss_low(void)  { GPIOB->BRR  = GPIO_BRR_BR2; }
static void spi1_nss_high(void) { GPIOB->BSRR = GPIO_BSRR_BS2; }

static uint8_t spi1_transfer(uint8_t tx) {
    uint32_t timeout = 10000;
    while (!(SPI1->SR & SPI_SR_TXE) && --timeout);
    if (!timeout) return 0xFF;
    *(volatile uint8_t*)&SPI1->DR = tx;
    timeout = 10000;
    while (!(SPI1->SR & SPI_SR_RXNE) && --timeout);
    if (!timeout) return 0xFF;
    return *(volatile uint8_t*)&SPI1->DR;
}

static void spi1_full_duplex(uint8_t *tx, uint8_t *rx, uint32_t len) {
    // 无间隙流水线: 一有 TXE 立刻预填下一字节, 帧内 SCK 连续 (等效原厂 DMA)。
    // 原厂用 soft-NSS, 传输期间不驱动 CS/PB2 → 不 toggle。
    (void)spi1_nss_low; (void)spi1_nss_high; (void)spi1_transfer;
    if (!len) return;
    volatile uint8_t *DR = (volatile uint8_t *)&SPI1->DR;
    uint32_t ti = 0, ri = 0, guard = 0;
    while (!(SPI1->SR & SPI_SR_TXE) && ++guard < 100000);
    *DR = tx[ti++];
    guard = 0;
    while (ri < len && ++guard < 2000000) {
        uint16_t sr = SPI1->SR;
        if (ti < len && (sr & SPI_SR_TXE)) { *DR = tx[ti++]; guard = 0; }
        if (sr & SPI_SR_RXNE)              { rx[ri++] = *DR; guard = 0; }
    }
}

/* ── DNB11xx 协议 (faithful port of 原厂 firmware/Sources/Driver_DNB11xx.c) ──
 *
 * 帧结构 (CreateSendBuf): [head 个 0x00][0x0F][ID][CMD|DataHi][DataMid][DataLo|CRC][ics*4 个 0xFF][0xF0]
 * 命令字 ulData 位域: [ID:8 (b24-31)][CMD:4 (b20-23)][Data:16 (b4-19)][CRC4:4 (b0-3)]
 * 菊花链响应延迟一帧 → 命令发两次(隔~2ms), 响应在 RecvBuf[head+4+(ID-1)*4] 起 4 字节 (big-endian ulData)。
 * CRC4: poly x^3+x^0 查表, 覆盖 ulData 的 nibble B7..B1 (跳过 B0=CRC)。
 */

#define DNB_CMD_ENUMERATE  0x00
#define DNB_CMD_INIT       0x01
#define DNB_CMD_SETMODE    0x0B
#define DNB_CMD_GETSTATUS  0x0D
#define DNB_CMD_GETDATA    0x0E

#define DNB_GS_CHECKID        0x00
#define DNB_STATUS_GENERAL    0x02
#define DNB_STATUS_SRVREQ     0x12

#define DNB_DATA_MAINVOLT     0x00
#define DNB_DATA_MAINDIETEMP  0x04
#define DNB_DATA_VZM          0x06

#define DNB_SETMODE_NORMAL    0x04

#define DNB_HEAD       8U      // 普通命令前导(0x00)字节数
#define DNB_HEAD_BOOT  430U    // 上电首帧长前导, flush 菊花链

static const uint8_t dnb_crc4_tab[16] = {
    0x00, 0x09, 0x0B, 0x02, 0x0F, 0x06, 0x04, 0x0D,
    0x07, 0x0E, 0x0C, 0x05, 0x08, 0x01, 0x03, 0x0A,
};

static uint8_t dnb_crc4(uint32_t ul) {
    uint8_t c = 0;
    c = dnb_crc4_tab[c ^ ((ul >> 28) & 0xF)];
    c = dnb_crc4_tab[c ^ ((ul >> 24) & 0xF)];
    c = dnb_crc4_tab[c ^ ((ul >> 20) & 0xF)];
    c = dnb_crc4_tab[c ^ ((ul >> 16) & 0xF)];
    c = dnb_crc4_tab[c ^ ((ul >> 12) & 0xF)];
    c = dnb_crc4_tab[c ^ ((ul >>  8) & 0xF)];
    c = dnb_crc4_tab[c ^ ((ul >>  4) & 0xF)];
    return c & 0xF;
}

static uint32_t dnb_make_cmd(uint8_t id, uint8_t cmd, uint16_t data) {
    uint32_t ul = ((uint32_t)id << 24) | ((uint32_t)(cmd & 0xF) << 20) | ((uint32_t)data << 4);
    return ul | dnb_crc4(ul);
}

static void dnb_delay_cycles(volatile uint32_t n) { while (n--) __asm volatile ("nop"); }

// 发命令, 返回 id 槽位的 4 字节响应组成的 ulData。head=前导字节数, send_two=发两次。
static uint32_t dnb_xfer(uint8_t id, uint8_t cmd, uint16_t data,
                         uint16_t head, uint8_t ics, uint8_t send_two) {
    uint32_t ul = dnb_make_cmd(id, cmd, data);
    uint32_t i = 0, j;
    if (head < 1) head = 1;
    if (head > DNB_BUFLEN - 8) head = DNB_BUFLEN - 8;

    for (; i < (uint32_t)head - 1; i++) dnb_tx[i] = 0x00;
    dnb_tx[i++] = 0x0F;
    dnb_tx[i++] = (uint8_t)(ul >> 24);  // ID
    dnb_tx[i++] = (uint8_t)(ul >> 16);  // CMD | Data[15:12]
    dnb_tx[i++] = (uint8_t)(ul >>  8);  // Data[11:4]
    dnb_tx[i++] = (uint8_t)(ul);        // Data[3:0] | CRC
    j = i;
    for (; (i - j) < (uint32_t)ics * 4; i++) dnb_tx[i] = 0xFF;
    dnb_tx[i++] = 0xF0;
    uint32_t len = i;

    spi1_full_duplex(dnb_tx, dnb_rx, len);
    if (send_two) { dnb_delay_cycles(16000); spi1_full_duplex(dnb_tx, dnb_rx, len); }

    uint32_t off = (id >= 250) ? ((uint32_t)head + 4)
                               : ((uint32_t)head + 4 + (uint32_t)(id ? id - 1 : 0) * 4);
    if (off + 3 >= DNB_BUFLEN) return 0;
    jcy_dnb_rx0 = ((uint16_t)dnb_rx[off]     << 8) | dnb_rx[off + 1];
    jcy_dnb_rx1 = ((uint16_t)dnb_rx[off + 2] << 8) | dnb_rx[off + 3];
    return ((uint32_t)dnb_rx[off]     << 24) | ((uint32_t)dnb_rx[off + 1] << 16) |
           ((uint32_t)dnb_rx[off + 2] <<  8) | (uint32_t)dnb_rx[off + 3];
}

static uint8_t dnb_resp_crc_ok(uint32_t ul) {
    return dnb_crc4(ul & ~0xFu) == (uint8_t)(ul & 0xF);
}

/* JCY8001 拓扑: 2 颗 DNB1101 — U6=SPI 网关(ID=1, 只有 TM), U8=测量(ID=2, VM/TM/ZM)。
 * 电芯电压/温度从测量芯片 ID=2 读取。 */
#define DNB_CHAIN_LEN  2   // 链上 IC 数 (网关 + 测量)
#define DNB_MEAS_ID    2   // U8 测量芯片 ID

/* 枚举菊花链: 逐个确认 ID; 未枚举(冷启动)则 Enum 分配 SetID=target。
 * 暖启动(ID 已存在)时 CheckID 直接返回对应 ID, 不重发 Enum (非破坏)。返回确认到的 IC 数。 */
static uint8_t dnb_enumerate(void) {
    uint8_t found = 0;
    for (uint8_t target = 1; target <= DNB_CHAIN_LEN; target++) {
        uint16_t head = (target == 1) ? DNB_HEAD_BOOT : DNB_HEAD;   // 首帧长前导 flush 菊花链
        uint32_t cf = dnb_xfer(target, DNB_CMD_GETSTATUS, DNB_GS_CHECKID, head, target, 1);
        uint8_t aid = (uint8_t)(cf >> 24);
        if (aid != target || !dnb_resp_crc_ok(cf)) {
            // Enum(ID=0 寻址未枚举 IC, SetID=target → Enum_Field.SetID=target → Data=target)
            dnb_xfer(0, DNB_CMD_ENUMERATE, target, DNB_HEAD, target, 1);
            dnb_delay_cycles(16000);
            cf  = dnb_xfer(target, DNB_CMD_GETSTATUS, DNB_GS_CHECKID, DNB_HEAD, target, 1);
            aid = (uint8_t)(cf >> 24);
        }
        if (aid == target) found++;
    }
    jcy_dnb_debug = found;
    return found;
}

/* ── Init: NrOfICsChain=nr, EnSrvReq=1 (bit12) → Data=0x1000|nr ── */
static void dnb_init_ic(uint8_t id, uint8_t nr_of_ics) {
    uint16_t data = (uint16_t)(nr_of_ics & 0xFF) | (1u << 12);
    dnb_xfer(id, DNB_CMD_INIT, data, DNB_HEAD, DNB_CHAIN_LEN, 1);
}

/* ── 读测量芯片 (ID=2) 的 GetData/GetStatus, 链长 ics=DNB_CHAIN_LEN ── */
static uint32_t dnb_get_data(uint8_t id, uint8_t data_type) {
    return dnb_xfer(id, DNB_CMD_GETDATA, data_type, DNB_HEAD, DNB_CHAIN_LEN, 1);
}

static uint32_t dnb_get_status(uint8_t id, uint8_t status_type) {
    return dnb_xfer(id, DNB_CMD_GETSTATUS, status_type, DNB_HEAD, DNB_CHAIN_LEN, 1);
}

/* ── 阻抗测量 (ZM) ──────────────────────────────────────────────────────── */
#define DNB_CMD_SETZMCURR  0x06   // 设置 ZM 电流/使能 (EnZM=bit11)
#define DNB_CMD_SETZMFREQ  0x07   // 设置 ZM 频率 (FRQMantissa|FRQExp<<8|LFNS<<12)
#define DNB_DATA_ZREAL     0x07   // GetData: 阻抗实部 (ZMantissa b4-15, ZExp b16-19)
#define DNB_DATA_ZIMAG     0x08   // GetData: 阻抗虚部
#define DNB_SRVREQ_BALZMDONE  (1u << 7)   // SrvReq bit7: 均衡/阻抗测量完成

/* 启动一次阻抗测量: SetZMFreq → SetZMCurr(EnZM=1)。发到测量芯片 U8。
 * HiPass(增益): jcy_zm_gain 1→00, 4→01, 16→10。
 * 低阻低频模式(jcy_zm_mode=1): 强制 LFNS=1(压低频噪声) + 增益16, 覆盖单独设置。 */
static void dnb_start_zm(void) {
    uint16_t freq   = jcy_zm_freq_set;
    uint8_t  hipass = (jcy_zm_gain >= 16) ? 0x2 : (jcy_zm_gain >= 4 ? 0x1 : 0x0);
    if (jcy_zm_mode == 1) {            // 低阻低频模式
        freq  |= (1u << 12);           // LFNS on
        hipass = 0x2;                  // gain16
    }
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETZMFREQ, freq, DNB_HEAD, DNB_CHAIN_LEN, 1);
    dnb_delay_cycles(8000);
    // EnZM=1(bit11), enXCS=1(bit10 外部电流源, JCY8001 用外部MOSFET, ZM必须外部源),
    // HiPass(bit8-9), ZMTimeOut=0x10
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETZMCURR,
             (1u << 11) | (1u << 10) | ((uint16_t)hipass << 8) | 0x10, DNB_HEAD, DNB_CHAIN_LEN, 1);
}

/* 关闭阻抗测量 (SetZMCurr EnZM=0) → 恢复正常电压测量 (VM)。 */
static void dnb_stop_zm(void) {
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETZMCURR, 0x0000, DNB_HEAD, DNB_CHAIN_LEN, 1);
}

/* ── 均衡 (Balance) ─────────────────────────────────────────────────────── */
#define DNB_CMD_SETBALCURR  0x08   // EnBal=bit12, PWM=bits8-11, BalTimeOut=bits0-7
#define DNB_CMD_SETBALVOLT  0x09   // BalMode=bit14, BalVolt=bits0-13 (0-16383 → 1.2-6.0V)

/* 启动均衡: SetBalVolt(模式+目标电压) → SetBalCurr(EnBal=1, PWM, 超时)。发到 U8。
 * 主机寄存器 jcy_bal_volt 0~255 (mV=1200+值*18.8) → 芯片 14位 BalVolt ≈ 值*64 (近似, 待标定)。 */
static void dnb_start_balance(void) {
    uint16_t chip_bv = (uint16_t)((uint32_t)jcy_bal_volt * 64);     // 0~255 → ~0..16320
    if (chip_bv > 0x3FFF) chip_bv = 0x3FFF;
    uint16_t bv = (uint16_t)((jcy_bal_mode ? (1u << 14) : 0) | (chip_bv & 0x3FFF));
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETBALVOLT, bv, DNB_HEAD, DNB_CHAIN_LEN, 1);
    dnb_delay_cycles(8000);
    uint16_t bc = (uint16_t)((1u << 12) | ((jcy_bal_pwm & 0x0F) << 8) | (jcy_bal_time & 0xFF));
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETBALCURR, bc, DNB_HEAD, DNB_CHAIN_LEN, 1);
}

/* 停止均衡 (SetBalCurr EnBal=0)。 */
static void dnb_stop_balance(void) {
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETBALCURR, 0x0000, DNB_HEAD, DNB_CHAIN_LEN, 1);
}

/* ── CRC16 Modbus ───────────────────────────────────────────────────────── */

static uint16_t crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}

/* ── Modbus Frame Processing ────────────────────────────────────────────── */

static uint8_t tx_buf[256];

static void modbus_reply(const uint8_t *data, uint16_t len) {
    uint16_t crc = crc16(data, len);
    for (uint16_t i = 0; i < len; i++) usart2_send(data[i]);
    usart2_send(crc & 0xFF); usart2_send(crc >> 8);
}

static void modbus_exception(uint8_t fc, uint8_t code) {
    uint8_t e[3] = { 0x01, (uint8_t)(fc | 0x80), code };
    modbus_reply(e, 3);
}

static void process_modbus(uint8_t *rx, uint16_t rxlen) {
    if (rxlen < 8 || rx[0] != 0x01) return;
    uint16_t crc = crc16(rx, rxlen - 2);
    if (crc != (rx[rxlen - 2] | (rx[rxlen - 1] << 8))) return;

    switch (rx[1]) {
        case 0x01: {
            uint16_t addr = (rx[2] << 8) | rx[3], count = (rx[4] << 8) | rx[5];
            if (count == 0 || count > 2000) { modbus_exception(rx[1], 0x03); return; }
            tx_buf[0] = 0x01; tx_buf[1] = 0x01; tx_buf[2] = (count + 7) / 8;
            uint8_t *p = &tx_buf[3]; *p = 0;
            for (uint16_t i = 0; i < count; i++) {
                if (i && (i % 8 == 0)) *(++p) = 0;
                if (addr + i == 0) *p |= 0x01;
            }
            modbus_reply(tx_buf, 3 + tx_buf[2]); break;
        }
        case 0x02: {
            uint16_t addr = (rx[2] << 8) | rx[3], count = (rx[4] << 8) | rx[5];
            if (count == 0 || count > 2000) { modbus_exception(rx[1], 0x03); return; }
            tx_buf[0] = 0x01; tx_buf[1] = 0x02; tx_buf[2] = (count + 7) / 8;
            uint8_t *p = &tx_buf[3]; *p = 0;
            for (uint16_t i = 0; i < count; i++) {
                if (i && (i % 8 == 0)) *(++p) = 0;
                if (addr + i == 0x1000) *p |= 0x00; // DI0 default low
            }
            modbus_reply(tx_buf, 3 + tx_buf[2]); break;
        }
        case 0x03: case 0x04: {
            uint16_t addr = (rx[2] << 8) | rx[3], count = (rx[4] << 8) | rx[5];
            if (count == 0 || count > 125) { modbus_exception(rx[1], 0x03); return; }
            tx_buf[0] = 0x01; tx_buf[1] = rx[1]; tx_buf[2] = count * 2;
            uint8_t *p = &tx_buf[3];
            for (uint16_t i = 0; i < count; i++) {
                uint16_t v = get_reg(addr + i);
                *p++ = v >> 8; *p++ = v & 0xFF;
            }
            modbus_reply(tx_buf, 3 + count * 2); break;
        }
        case 0x05: {   // 写单个线圈: 0x0000/0x0F00 启ZM, 0x0040/0x0F01 启均衡, 0x0080/0x0F02 模式
            uint16_t addr = (rx[2] << 8) | rx[3], val = (rx[4] << 8) | rx[5];
            uint8_t on = (val == 0xFF00);
            if      (addr == 0x0000 || addr == 0x0F00) zm_start_req  = on;
            else if (addr == 0x0040 || addr == 0x0F01) bal_start_req = on;
            else if (addr == 0x0080 || addr == 0x0F02) jcy_bal_mode  = on;
            modbus_reply(rx, 6); break;   // FC05 回显请求
        }
        case 0x06: {
            uint16_t addr = (rx[2] << 8) | rx[3], val = (rx[4] << 8) | rx[5];
            set_reg(addr, val); modbus_reply(rx, 6); break;
        }
        case 0x0F: {   // 写多个线圈
            uint16_t addr = (rx[2] << 8) | rx[3], qty = (rx[4] << 8) | rx[5];
            uint8_t bc = rx[6];
            if (qty == 0 || qty > 1968 || bc != (qty + 7) / 8 || rxlen < 9 + bc) {
                modbus_exception(0x0F, 0x03); return;
            }
            for (uint16_t i = 0; i < qty; i++) {
                uint8_t bit = (rx[7 + i / 8] >> (i % 8)) & 1;
                uint16_t a = addr + i;
                if      (a == 0x0000 || a == 0x0F00) zm_start_req  = bit;
                else if (a == 0x0040 || a == 0x0F01) bal_start_req = bit;
                else if (a == 0x0080 || a == 0x0F02) jcy_bal_mode  = bit;
            }
            tx_buf[0]=0x01; tx_buf[1]=0x0F; tx_buf[2]=rx[2]; tx_buf[3]=rx[3]; tx_buf[4]=rx[4]; tx_buf[5]=rx[5];
            modbus_reply(tx_buf, 6); break;
        }
        case 0x10: {   // 写多个寄存器
            uint16_t addr = (rx[2] << 8) | rx[3], qty = (rx[4] << 8) | rx[5];
            uint8_t bc = rx[6];
            if (qty == 0 || qty > 123 || bc != qty * 2 || rxlen < 9 + bc) {
                modbus_exception(0x10, 0x03); return;
            }
            for (uint16_t i = 0; i < qty; i++)
                set_reg(addr + i, (rx[7 + i * 2] << 8) | rx[8 + i * 2]);
            tx_buf[0]=0x01; tx_buf[1]=0x10; tx_buf[2]=rx[2]; tx_buf[3]=rx[3]; tx_buf[4]=rx[4]; tx_buf[5]=rx[5];
            modbus_reply(tx_buf, 6); break;
        }
        default:
            modbus_exception(rx[1], 0x01); break;   // 不支持的功能码 → 异常 0x01
    }
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    RCC->CFGR = 0x00000000;
    RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_PLLON);
    RCC->CR |= RCC_CR_HSION;
    SystemCoreClock = 8000000;

    init_registers();
    usart2_init();
    spi1_init();

    // 枚举菊花链 (U6 网关=ID1, U8 测量=ID2), 并 Init 两颗 IC (链长=2)。
    // U8 由硬件 SPI_En=低固定在测量模式, 唤醒后自动进入正常模式持续测量 VM/TM。
    dnb_ic_count = dnb_enumerate();
    if (dnb_ic_count == 0) dnb_ic_count = DNB_CHAIN_LEN;   // 容错: 按已知拓扑继续
    jcy_ch_count = 1;                                      // 对外 1 个测量通道 (U8)
    dnb_init_ic(1, DNB_CHAIN_LEN);
    dnb_delay_cycles(8000);
    dnb_init_ic(DNB_MEAS_ID, DNB_CHAIN_LEN);
    dnb_delay_cycles(80000);
    jcy_status = 0x0001;

    uint8_t  frame_buf[256];
    uint16_t frame_idx = 0;
    uint32_t idle_ticks = 0;
    uint32_t measure_ticks = 0;
    uint8_t  zm_running = 0;        // 阻抗测量进行中
    uint16_t zm_cycles  = 0;        // ZM 轮询周期计数 (超时保护)
    uint8_t  bal_running = 0;       // 均衡进行中
#define ZM_TIMEOUT_CYCLES  12       // ~6s 没完成则放弃, 关 EnZM 恢复 VM

#define MEASURE_INTERVAL  50000   // ~500ms @ 8MHz

    while (1) {
        if (USART2->SR & USART_SR_RXNE) {
            uint8_t c = USART2->DR;
            if (frame_idx < sizeof(frame_buf)) frame_buf[frame_idx++] = c;
            idle_ticks = 0;
        } else {
            idle_ticks++;
            measure_ticks++;

            if (frame_idx >= 8 && idle_ticks > 5000) {
                process_modbus(frame_buf, frame_idx);
                frame_idx = 0;
            }

            // DNB1101 定期测量: 从测量芯片 U8(ID=2) 读温度+电压。命令间留 ~4ms 间隔。
            if (dnb_ic_count > 0 && measure_ticks >= MEASURE_INTERVAL) {
                measure_ticks = 0;

                uint32_t gs = dnb_get_status(DNB_MEAS_ID, DNB_STATUS_GENERAL);
                jcy_dnb_gs_hi = (uint16_t)(gs >> 16); jcy_dnb_gs_lo = (uint16_t)gs;
                dnb_delay_cycles(8000);

                // MainDieTemp: 12-bit 有符号 (b4-15), T(℃)=raw*0.0625, 寄存器 0x3300=T*10=raw*5/8
                uint32_t ut = dnb_get_data(DNB_MEAS_ID, DNB_DATA_MAINDIETEMP);
                int16_t t12 = (int16_t)((ut >> 4) & 0x0FFF);
                if (t12 & 0x0800) t12 |= (int16_t)0xF000;
                jcy_dnb_temp_raw = (uint16_t)t12;
                jcy_temp = (uint16_t)((int32_t)t12 * 5 / 8);
                dnb_delay_cycles(8000);

                // MainVolt: 14-bit (b4-17), V=raw/16383*4.8+1.2, 寄存器 0x3340=V*10000
                uint32_t uv = dnb_get_data(DNB_MEAS_ID, DNB_DATA_MAINVOLT);
                uint16_t f = (uint16_t)((uv >> 4) & 0x3FFF);
                jcy_dnb_volt_raw = f;
                jcy_voltage = (uint16_t)(((uint32_t)f * 48000UL) / 16383UL + 12000UL);

                // 阻抗测量 (ZM): FC05 线圈触发 → 启动 → 轮询 BalZMDone → 读 Zreal/Zimag/VZM
                if (zm_start_req) {
                    zm_start_req = 0;
                    jcy_zm_done = 0;
                    jcy_status = 0x0001;        // ZM 测量中
                    dnb_delay_cycles(8000);
                    dnb_start_zm();
                    zm_running = 1;
                    zm_cycles  = 0;
                } else if (zm_running) {
                    dnb_delay_cycles(8000);
                    uint32_t ss = dnb_get_status(DNB_MEAS_ID, DNB_STATUS_SRVREQ);
                    jcy_dnb_ss_hi = (uint16_t)(ss >> 16); jcy_dnb_ss_lo = (uint16_t)ss;
                    uint16_t flags = (uint16_t)((ss >> 4) & 0xFFFF);
                    if (flags & DNB_SRVREQ_BALZMDONE) {        // 测量完成 → 读结果
                        dnb_delay_cycles(8000);
                        jcy_zm_re  = (uint16_t)((dnb_get_data(DNB_MEAS_ID, DNB_DATA_ZREAL) >> 4) & 0xFFFF);
                        dnb_delay_cycles(8000);
                        jcy_zm_im  = (uint16_t)((dnb_get_data(DNB_MEAS_ID, DNB_DATA_ZIMAG) >> 4) & 0xFFFF);
                        dnb_delay_cycles(8000);
                        jcy_zm_vzm = (uint16_t)((dnb_get_data(DNB_MEAS_ID, DNB_DATA_VZM)   >> 4) & 0xFFFF);
                        // 换算成 μΩ (64位×100000), 频率索引由 0x4200 的 M/E 反查
                        {
                            int idx = dnb_zm_index((uint8_t)(jcy_zm_freq_set & 0xFF),
                                                   (uint8_t)((jcy_zm_freq_set >> 8) & 0x0F));
                            long long zr = 0, zi = 0;
                            dnb_zm_convert(jcy_zm_re, jcy_zm_im, jcy_zm_vzm,
                                           (uint8_t)jcy_samp_res, idx, &zr, &zi);
                            jcy_zm_re64 = zr;
                            jcy_zm_im64 = zi;
                        }
                        jcy_zm_done = 1;
                        jcy_status  = 0x0006;   // 测量完成
                        dnb_delay_cycles(8000);
                        dnb_stop_zm();          // 关 EnZM, 恢复 VM
                        zm_running = 0;
                    } else if (++zm_cycles >= ZM_TIMEOUT_CYCLES) {   // 超时放弃
                        dnb_delay_cycles(8000);
                        dnb_stop_zm();          // 关 EnZM, 恢复 VM (否则 VM 一直被压住读 0)
                        zm_running = 0;
                        jcy_status = 0x0005;    // 硬件/ADC 错误 (无有效电芯无法测阻抗)
                    }
                } else {
                    jcy_status = 0x0000;        // 空闲
                }

                // 均衡: 线圈 0x0040 启动/停止 (与 VM 共存, 芯片按 BalTimeOut 自动结束)
                if (bal_start_req && !bal_running) {
                    dnb_delay_cycles(8000); dnb_start_balance(); bal_running = 1;
                    jcy_status = 0x0002;        // 均衡运行中
                } else if (!bal_start_req && bal_running) {
                    dnb_delay_cycles(8000); dnb_stop_balance(); bal_running = 0;
                }
            }
        }
    }
}
