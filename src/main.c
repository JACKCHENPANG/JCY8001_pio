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
volatile uint16_t jcy_reenum_count;  // DNB 自动重枚举次数 (自愈计数)
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
volatile uint16_t jcy_zm_fast;       // 0x4340 ZM 速度: 0=标准(转换门余量足,稳), 1=快速(余量紧,~省一半时间)
volatile uint16_t jcy_zm_convovr;    // 0x4360 调试: 转换门周期数覆盖(0=按频率自动). 用于标定最低收敛周期
volatile uint16_t jcy_autorange;     // 0x4380 自动挡: 0=关(用jcy_samp_res), 1=开(扫频第1点用10Ω探, 按激励扰动选档锁定)
// ── 固件自主扫频 (EIS sweep): 上位机一次性下发频点表→触发→固件逐点测→边跑可边读/完成批量读 ──
#define SWEEP_MAX  64                  // 最大频点数
volatile uint16_t jcy_sweep_freq[SWEEP_MAX]; // 0x4400+idx 频点表 (16位 M/E 字, 同 0x4200 格式)
volatile uint16_t jcy_sweep_count;           // 0x43C0 频点数 N (1~64)
volatile uint16_t jcy_sweep_state;           // 0x3E40 状态 0=空闲 1=跑中 2=完成 3=中止
volatile uint16_t jcy_sweep_idx;             // 0x3E41 已完成点数 (= 下一个待测索引)
volatile int64_t  jcy_sweep_re[SWEEP_MAX];   // 0x3400+idx*4 各点实部 (×100000=μΩ)
volatile int64_t  jcy_sweep_im[SWEEP_MAX];   // 0x3500+idx*4 各点虚部
volatile uint16_t jcy_sweep_vzm[SWEEP_MAX];  // 0x3600+idx 各点 VZM 原始
volatile uint16_t jcy_sweep_fecho[SWEEP_MAX];// 0x3640+idx 各点实际所用频率码 (M/E)
volatile uint8_t  sweep_start_req;           // coil 0x00C0 电平: 1=启动/进行 0=停止
volatile uint8_t  sweep_running;             // 扫频进行中 (内部)

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
    jcy_zm_avg     = 1;   // ZM平均次数(1=不平均). >1则多次测量取平均压噪, N倍耗时
    jcy_fw_version = 0x0223;   // v2.23 (加设备唯一序列号 UID 寄存器 0x3E10-15)
    jcy_git_rev    = 0x0001;
    jcy_build_date = 0x0602;   // 2026-06-02 (MMDD)
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
    jcy_zm_fast = 0;
    jcy_zm_convovr = 0;
    jcy_sweep_count = 0; jcy_sweep_state = 0; jcy_sweep_idx = 0;
    sweep_start_req = 0; sweep_running = 0;
}

static uint16_t get_reg(uint16_t addr) {
    // ── 扫频结果块 (64位 RE/IM 大端, 每点4寄存器; VZM/频率码 每点1寄存器) ──
    if (addr >= 0x3400 && addr <= 0x34FF) { uint16_t i=(addr-0x3400)>>2, s=(addr-0x3400)&3; return (uint16_t)(jcy_sweep_re[i] >> ((3-s)*16)); }
    if (addr >= 0x3500 && addr <= 0x35FF) { uint16_t i=(addr-0x3500)>>2, s=(addr-0x3500)&3; return (uint16_t)(jcy_sweep_im[i] >> ((3-s)*16)); }
    if (addr >= 0x3600 && addr <= 0x363F) return jcy_sweep_vzm[addr-0x3600];
    if (addr >= 0x3640 && addr <= 0x367F) return jcy_sweep_fecho[addr-0x3640];
    if (addr >= 0x4400 && addr <= 0x443F) return jcy_sweep_freq[addr-0x4400];
    /* 设备唯一序列号 = STM32 出厂 96位 UID (0x1FFFF7E8), 6 个寄存器 0x3E10..0x3E15 */
    if (addr >= 0x3E10 && addr <= 0x3E15) return *(volatile uint16_t*)(0x1FFFF7E8u + (uint32_t)(addr-0x3E10)*2u);
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
        case 0x3E30: return jcy_reenum_count;  // DNB 自愈重枚举次数
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
        case 0x4340: return jcy_zm_fast;          // ZM 速度 0=标准 1=快速
        case 0x4360: return jcy_zm_convovr;       // 转换门覆盖(调试)
        case 0x4380: return jcy_autorange;        // 自动挡 0=关 1=开
        case 0x43C0: return jcy_sweep_count;      // 扫频频点数 N
        case 0x3E40: return jcy_sweep_state;      // 扫频状态 0空闲/1跑中/2完成/3中止
        case 0x3E41: return jcy_sweep_idx;        // 已完成点数
        case 0x40D0: return (uint16_t)(dnb_zm_get_resis(0)*1000.0f+0.5f);  // 10Ω档实际阻值(mΩ)
        case 0x40D1: return (uint16_t)(dnb_zm_get_resis(1)*1000.0f+0.5f);  // 5Ω档(mΩ)
        case 0x40D2: return (uint16_t)(dnb_zm_get_resis(2)*1000.0f+0.5f);  // 1Ω档(mΩ)
        default:     return 0x0000;
    }
}

/* ── 采样电阻校准持久化 (Flash 末页 0x0803F800, F103RC 2KB/页) ── */
#define CAL_FLASH_ADDR  0x0803F800UL
#define CAL_MAGIC       0xCA1B
static void flash_save_cal(void){
    uint16_t d[4];
    d[0]=CAL_MAGIC;
    d[1]=(uint16_t)(dnb_zm_get_resis(0)*1000.0f+0.5f);  // 10Ω档 mΩ
    d[2]=(uint16_t)(dnb_zm_get_resis(1)*1000.0f+0.5f);  // 5Ω档
    d[3]=(uint16_t)(dnb_zm_get_resis(2)*1000.0f+0.5f);  // 1Ω档
    FLASH->KEYR=0x45670123; FLASH->KEYR=0xCDEF89AB;     // 解锁 FPEC
    while(FLASH->SR & FLASH_SR_BSY);
    FLASH->CR|=FLASH_CR_PER; FLASH->AR=CAL_FLASH_ADDR; FLASH->CR|=FLASH_CR_STRT;
    while(FLASH->SR & FLASH_SR_BSY); FLASH->CR&=~FLASH_CR_PER;   // 擦除该页
    for(int i=0;i<4;i++){
        FLASH->CR|=FLASH_CR_PG;
        *(volatile uint16_t*)(CAL_FLASH_ADDR+i*2u)=d[i];
        while(FLASH->SR & FLASH_SR_BSY); FLASH->CR&=~FLASH_CR_PG;
    }
    FLASH->CR|=FLASH_CR_LOCK;
}
static void flash_load_cal(void){
    const volatile uint16_t *p=(const volatile uint16_t*)CAL_FLASH_ADDR;
    if(p[0]!=CAL_MAGIC) return;          // 无有效标定 → 用默认 10/5/1Ω
    for(int i=0;i<3;i++){
        uint16_t m=p[1+i];
        if(m>50 && m<60000) dnb_zm_set_resis(i,(float)m/1000.0f);
    }
}

static void set_reg(uint16_t addr, uint16_t val) {
    if (addr >= 0x4400 && addr <= 0x443F) { jcy_sweep_freq[addr-0x4400] = val; return; }  // 写频点表
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
        case 0x40D0: dnb_zm_set_resis(0,(float)val/1000.0f); break;  // 校准 10Ω档 (mΩ)
        case 0x40D1: dnb_zm_set_resis(1,(float)val/1000.0f); break;  // 校准 5Ω档
        case 0x40D2: dnb_zm_set_resis(2,(float)val/1000.0f); break;  // 校准 1Ω档
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
        case 0x4340: jcy_zm_fast  = val; break;              // ZM 速度 0=标准 1=快速
        case 0x4360: jcy_zm_convovr = val; break;            // 转换门覆盖(调试)
        case 0x4380: jcy_autorange  = val; break;            // 自动挡开关
        case 0x4F0A: jcy_zm_mode  = val; break;              // 群发 ZM 测量模式
        case 0x43C0: jcy_sweep_count = (val > SWEEP_MAX) ? SWEEP_MAX : val; break;  // 扫频频点数 N
    }
}

/* ── 自动挡: 用探测点(10Ω, 中低频~9.5Hz, |Z|接近最大)的 RE/IM/VZM, 按激励扰动 I·|Z|≤15mV 选最小采样电阻 ──
 * |Z|=√(re²+im²)·1e-6 Ω (re/im 为 μΩ); I=VZM/R; 取扰动达标的最小 R(电流最大/SNR好)。
 * 都不达标→10Ω。R 取可标定的 dnb_zm_get_resis(sel)。返回 sel(0=10Ω 1=5Ω 2=1Ω)。
 * ⚠️必须用中低频探(高频|Z|只是电芯一部分, 会低估→选档偏小→低频端过驱)。 */
static uint8_t autorange_probing = 0;          /* 正在测探测点(非扫频点) */
#define AUTORANGE_PROBE_FREQ 0x070A            /* ~9.5Hz 探测频率 */
static uint8_t autorange_pick(long long re, long long im, uint16_t vzm) {
    long long re_u = re / 100000, im_u = im / 100000;          /* μΩ */
    double s2 = (double)re_u * re_u + (double)im_u * im_u;      /* μΩ² */
    if (!(s2 > 0.0)) return 0;                                  /* 探测失败/无信号 → 安全用 10Ω */
    double vzmV = (vzm * 4800.0 / 16383.0 + 1200.0) / 1000.0;  /* V */
    const uint8_t order[3] = { 2, 1, 0 };                       /* 1Ω,5Ω,10Ω: 小R(大电流)优先 */
    const double target2 = 0.015 * 0.015;                       /* (15mV)² */
    for (int k = 0; k < 3; k++) {
        uint8_t s = order[k];
        double R = (double)dnb_zm_get_resis(s);
        double pert2 = (vzmV / R) * (vzmV / R) * s2 * 1e-12;    /* (I·|Z|)² */
        if (pert2 <= target2) return s;
    }
    return 0;   /* 10Ω 仍过驱 → 用10Ω */
}

/* ── 扫频: 一个频点测完(成功或ADC错)后调用: 存结果→推进到下一点或收尾 ── */
static void sweep_on_point_done(long long re, long long im, uint16_t vzm) {
    if (!sweep_running) return;
    /* 自动挡: 这是探测点(10Ω@~9.5Hz)的结果 → 定档锁定, 然后正式从第0点开始扫(不存探测点) */
    if (autorange_probing) {
        autorange_probing = 0;
        jcy_samp_res = autorange_pick(re, im, vzm);
        jcy_sweep_idx   = 0;
        jcy_zm_freq_set = jcy_sweep_freq[0];
        zm_start_req    = 1;
        return;
    }
    uint16_t i = jcy_sweep_idx;
    if (i < jcy_sweep_count && i < SWEEP_MAX) {
        jcy_sweep_re[i]   = re;
        jcy_sweep_im[i]   = im;
        jcy_sweep_vzm[i]  = vzm;
        jcy_sweep_fecho[i]= jcy_zm_freq_set;     // 本点实际所用频率码
    }
    jcy_sweep_idx = i + 1;                        // 已完成点数 +1 (上位机可据此边跑边读)
    if (jcy_sweep_idx < jcy_sweep_count) {        // 还有点 → 载入下一频点并触发单点 ZM
        jcy_zm_freq_set = jcy_sweep_freq[jcy_sweep_idx];
        zm_start_req = 1;
    } else {                                      // 全部完成
        sweep_running   = 0;
        jcy_sweep_state = 2;
    }
}

/* ── USART2 ─────────────────────────────────────────────────────────────── */

/* RX ring buffer fed by USART2 RXNE interrupt.
 * Why: at 8 MHz the bare-metal super-loop spends tens of ms inside one DNB
 * measurement burst. The old code polled RXNE once per loop iteration, so any
 * byte arriving during a DNB burst was lost (single DR, no FIFO) before the
 * next poll → corrupt frame → CRC fail → silent drop → Modbus never replied.
 * The ISR now captures every byte regardless of how slow the loop is; the loop
 * just drains the ring and does Modbus 3.5-char idle-gap framing as before. */
#define RXR_SZ 512u                       /* power of two */
static volatile uint8_t  rxr_buf[RXR_SZ];
static volatile uint16_t rxr_head = 0;    /* ISR writes */
static volatile uint16_t rxr_tail = 0;    /* main loop reads */

void USART2_IRQHandler(void) {
    /* Reading SR then DR also clears ORE (overrun) so it can never wedge RX. */
    uint32_t sr = USART2->SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        uint8_t c = (uint8_t)USART2->DR;
        uint16_t nh = (uint16_t)((rxr_head + 1u) & (RXR_SZ - 1u));
        if (nh != rxr_tail) {             /* drop on overflow rather than clobber */
            rxr_buf[rxr_head] = c;
            rxr_head = nh;
        }
    }
}

/* Pop one byte from the ring. Returns 1 if a byte was available. */
static int usart2_rx_pop(uint8_t *c) {
    if (rxr_head == rxr_tail) return 0;
    *c = rxr_buf[rxr_tail];
    rxr_tail = (uint16_t)((rxr_tail + 1u) & (RXR_SZ - 1u));
    return 1;
}

static void usart2_send(uint8_t c) {
    while (!(USART2->SR & USART_SR_TXE));
    USART2->DR = c;
}

static void usart2_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    GPIOA->CRL = (GPIOA->CRL & 0xFFFF0000) | 0x00004B04;
    USART2->BRR = 0x0045;
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    NVIC_SetPriority(USART2_IRQn, 1);     /* must run while loop is busy in DNB */
    NVIC_EnableIRQ(USART2_IRQn);
}

/* ── USART1 (J12 "UART" 接口, PA9=TX/PA10=RX) ───────────────────────────────
 * 接透传蓝牙模块: 手机当 "Modbus over BLE" 客户端, 读固件算好的 RE/IM 等寄存器。
 * 跟 USART2(CP2102)同一套 Modbus, 独立帧缓冲/中断, 回复按来源端口分流。
 * 波特率默认 115200(@8MHz BRR=0x45), 蓝牙模块需配成 115200。 */
static volatile uint8_t  rxr1_buf[RXR_SZ];
static volatile uint16_t rxr1_head = 0;
static volatile uint16_t rxr1_tail = 0;
static volatile uint8_t  g_reply_port = 2;   /* 当前处理帧的来源: 1=USART1(BLE) 2=USART2(USB) */

void USART1_IRQHandler(void) {
    uint32_t sr = USART1->SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        uint8_t c = (uint8_t)USART1->DR;
        uint16_t nh = (uint16_t)((rxr1_head + 1u) & (RXR_SZ - 1u));
        if (nh != rxr1_tail) { rxr1_buf[rxr1_head] = c; rxr1_head = nh; }
    }
}

static int usart1_rx_pop(uint8_t *c) {
    if (rxr1_head == rxr1_tail) return 0;
    *c = rxr1_buf[rxr1_tail];
    rxr1_tail = (uint16_t)((rxr1_tail + 1u) & (RXR_SZ - 1u));
    return 1;
}

static void usart1_send(uint8_t c) {
    while (!(USART1->SR & USART_SR_TXE));
    USART1->DR = c;
}

static void usart1_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN;
    /* PA9 = AF push-pull 50MHz (0xB), PA10 = floating input (0x4) */
    GPIOA->CRH = (GPIOA->CRH & 0xFFFFF00F) | 0x000004B0;
    USART1->BRR = 0x45;                   /* 115200 @ PCLK2=8MHz (匹配 JDY-10 出厂默认 UART, 模块免配) */
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    NVIC_SetPriority(USART1_IRQn, 1);
    NVIC_EnableIRQ(USART1_IRQn);
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

/* ── 采样电阻量程选通 (硬件: R6=1Ω←PB5, R7=5Ω←PB3, R29=10Ω←PD2; 互斥, 高=选通) ──
 * PB3 是 JTDO, 需 SWJ_CFG=010 关 JTAG【保留 SWD】才能当 GPIO (J-Link 仍可烧)。 */
static void resis_gpio_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPDEN | RCC_APB2ENR_AFIOEN;
    // SWJ_CFG=010: 关 JTAG-DP, 保留 SW-DP。务必是 0x2 (绝不能 0x4=同时关 SWD 会变砖)
    AFIO->MAPR = (AFIO->MAPR & ~(0x7u << 24)) | (0x2u << 24);
    // PB3(12-15), PB5(20-23), PB6(24-27), PB7(28-31) 输出推挽 2MHz=0x2
    GPIOB->CRL = (GPIOB->CRL & ~((0xFu<<12)|(0xFu<<20)|(0xFu<<24)|(0xFu<<28)))
               | (0x2u<<12)|(0x2u<<20)|(0x2u<<24)|(0x2u<<28);
    // PD2(CRL 8-11) 输出推挽
    GPIOD->CRL = (GPIOD->CRL & ~(0xFu << 8)) | (0x2u << 8);
    GPIOB->BRR = (1u << 3);                          // PB3 默认关
    GPIOB->BSRR = (1u << 5) | (1u << 6) | (1u << 7); // PB5/PB6/PB7 常高 (匹配原厂 idle 0x..F0)
    GPIOD->BRR = (1u << 2);
}

/* 选采样电阻档: 0=10Ω(R29/PD2), 1=5Ω(R7/PB3), 2=1Ω(R6/PB5)。互斥。 */
static void resis_select(uint8_t sel) {
    GPIOB->BRR = (1u << 3) | (1u << 5);   // 先全关 PB3/PB5
    GPIOD->BRR = (1u << 2);                // 关 PD2
    if (sel == 0)      GPIOD->BSRR = (1u << 2);   // 10Ω R29 PD2
    else if (sel == 1) GPIOB->BSRR = (1u << 3);   //  5Ω R7  PB3
    else               GPIOB->BSRR = (1u << 5);   //  1Ω R6  PB5
}

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

/* ── 原厂 boot 额外配置 (JCY8001 源 DNB11xxThread, 在 Init 后): SetThVolt + SetThTemp。
 * 把电压/温度保护阈值设到最宽 (原厂值), 否则默认阈值可能把电芯判越界→芯片进保护→ZM 不完成(BalZMDone 不置位)。
 * 原厂: SetThVolt(0xff,Over=0xff,Under=0)=Data 0xFF00; SetThTemp(0xff,Over=0x7f,Under=0x80)=Data 0x7F80。 */
#define DNB_CMD_SETTHVOLT  0x03
#define DNB_CMD_SETTHTEMP  0x04
static void dnb_set_thresholds(void) {
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETTHVOLT, 0xFF00, DNB_HEAD, DNB_CHAIN_LEN, 1);
    dnb_delay_cycles(8000);
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETTHTEMP, 0x7F80, DNB_HEAD, DNB_CHAIN_LEN, 1);
    dnb_delay_cycles(8000);
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
#define DNB_CMD_SETSRVREQMASK 0x0A // 设置服务请求掩码 (原厂ZM前置, 抓包 data=0x75FF)
#define DNB_DATA_ZREAL     0x07   // GetData: 阻抗实部 (ZMantissa b4-15, ZExp b16-19)
#define DNB_DATA_ZIMAG     0x08   // GetData: 阻抗虚部
#define DNB_SRVREQ_BALZMDONE  (1u << 7)   // SrvReq flags bit7: 均衡/阻抗测量完成
#define DNB_SRVREQ_VMADCERR   (1u << 2)   // flags bit2: VM ADC 错误
#define DNB_SRVREQ_ZMADCERR   (1u << 8)   // flags bit8: ZM ADC 错误

/* 启动一次阻抗测量: SetZMFreq → SetZMCurr(EnZM=1)。发到测量芯片 U8。
 * HiPass(增益): jcy_zm_gain 1→00, 4→01, 16→10。
 * 低阻低频模式(jcy_zm_mode=1): 强制 LFNS=1(压低频噪声) + 增益16, 覆盖单独设置。 */
static void dnb_start_zm(void) {
    uint16_t freq   = jcy_zm_freq_set;
    uint8_t  hipass = (jcy_zm_gain >= 16) ? 0x2 : (jcy_zm_gain >= 4 ? 0x1 : 0x0);
    if (jcy_zm_mode == 1) {            // 低阻低频模式: LFNS + gain16 (与增益独立)
        freq  |= (1u << 12);           // LFNS on
        hipass = 0x2;                  // gain16
    }
    resis_select((uint8_t)jcy_samp_res);   // 选通采样电阻 (把对应档接进 ZM 电流回路)
    dnb_delay_cycles(8000);
    // ── 原厂 ZM 前置序列 (逻辑分析仪抓包确认, 发 ID=2): SetSrvReqMask → INIT → SetMode(Normal) ──
    // 缺这三条则 SetZMCurr 后报 CurrErr (激励电流建立不起来). 数值复制原厂抓包.
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETSRVREQMASK, 0x75FF, DNB_HEAD, DNB_CHAIN_LEN, 1);
    dnb_delay_cycles(8000);
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_INIT, 0x1009, DNB_HEAD, DNB_CHAIN_LEN, 1);
    dnb_delay_cycles(8000);
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETMODE, DNB_SETMODE_NORMAL, DNB_HEAD, DNB_CHAIN_LEN, 1);
    dnb_delay_cycles(8000);
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETZMFREQ, freq, DNB_HEAD, DNB_CHAIN_LEN, 1);
    dnb_delay_cycles(8000);
    // EnZM=1(bit11), enXCS=1(bit10 外部电流源, JCY8001 用外部MOSFET, ZM必须外部源),
    // HiPass(bit8-9), ZMTimeOut=0x10
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETZMCURR,
             (1u << 11) | (1u << 10) | ((uint16_t)hipass << 8) | 0x01, DNB_HEAD, DNB_CHAIN_LEN, 1);  // ZMTimeOut=1 (匹配原厂)
}

/* 关闭阻抗测量 (SetZMCurr EnZM=0) → 恢复正常电压测量 (VM)。同时关断采样电阻选通脚(安全)。 */
static void dnb_stop_zm(void) {
    dnb_xfer(DNB_MEAS_ID, DNB_CMD_SETZMCURR, 0x0000, DNB_HEAD, DNB_CHAIN_LEN, 1);
    GPIOB->BRR = (1u << 3) | (1u << 5);   // 关 PB3/PB5
    GPIOD->BRR = (1u << 2);                // 关 PD2 (不长时间驱动充/放电脚)
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
    void (*tx)(uint8_t) = (g_reply_port == 1) ? usart1_send : usart2_send;
    for (uint16_t i = 0; i < len; i++) tx(data[i]);
    tx(crc & 0xFF); tx(crc >> 8);
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
            else if (addr == 0x00C0)                   sweep_start_req = on;  // 扫频 启/停
            else if (addr == 0x0040 || addr == 0x0F01) bal_start_req = on;
            else if (addr == 0x0080 || addr == 0x0F02) jcy_bal_mode  = on;
            else if (addr == 0x0010 && on)               flash_save_cal();   // 保存采样电阻校准到Flash
            else if (addr == 0x0020 && on) {              // OTA: 进入 Bootloader 在线升级
                *(volatile uint32_t*)0x4002101Cu |= (1u<<28)|(1u<<27); // RCC_APB1ENR PWREN|BKPEN
                *(volatile uint32_t*)0x40007000u |= (1u<<8);           // PWR_CR DBP 解锁备份域
                *(volatile uint32_t*)0x40006C04u  = 0xB007u;           // BKP_DR1 = 升级标志
                modbus_reply(rx, 6);                                   // 先回复上位机
                for (volatile uint32_t i=0;i<300000;i++){}             // 等串口发完
                *(volatile uint32_t*)0xE000ED0Cu = 0x05FA0004u;        // 系统复位 → 进Bootloader
            }
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
                else if (a == 0x00C0)                sweep_start_req = bit;  // 扫频 启/停
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
    SCB->VTOR = 0x08004000u;   // OTA: App 重定位到 0x08004000, 中断向量表偏移
    RCC->CFGR = 0x00000000;
    RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_PLLON);
    RCC->CR |= RCC_CR_HSION;
    SystemCoreClock = 8000000;

    init_registers();
    flash_load_cal();   // 从Flash加载采样电阻校准(若有), 覆盖默认10/5/1Ω
    usart2_init();
    usart1_init();      // J12 透传蓝牙模块: Modbus over BLE (与 USART2 同协议)
    spi1_init();
    resis_gpio_init();   // 采样电阻量程选通 GPIO (PB5/PB3/PD2)

    // 枚举菊花链 (U6 网关=ID1, U8 测量=ID2), 并 Init 两颗 IC (链长=2)。
    // U8 由硬件 SPI_En=低固定在测量模式, 唤醒后自动进入正常模式持续测量 VM/TM。
    dnb_ic_count = dnb_enumerate();
    if (dnb_ic_count == 0) dnb_ic_count = DNB_CHAIN_LEN;   // 容错: 按已知拓扑继续
    jcy_ch_count = 1;                                      // 对外 1 个测量通道 (U8)
    dnb_init_ic(1, DNB_CHAIN_LEN);
    dnb_delay_cycles(8000);
    dnb_init_ic(DNB_MEAS_ID, DNB_CHAIN_LEN);
    dnb_delay_cycles(8000);
    dnb_set_thresholds();        // 原厂 boot: 设宽电压/温度保护阈值(防默认阈值误触发保护挡住 ZM)
    dnb_delay_cycles(80000);
    jcy_status = 0x0001;

    uint8_t  frame_buf[256];
    uint16_t frame_idx = 0;
    uint32_t idle_ticks = 0;
    uint8_t  frame_buf1[256];       // USART1/BLE 独立帧缓冲
    uint16_t frame_idx1 = 0;
    uint32_t idle_ticks1 = 0;
    uint32_t measure_ticks = 0;
    uint8_t  zm_running = 0;        // 阻抗测量进行中
    uint16_t zm_cycles  = 0;        // ZM 轮询周期计数 (超时保护)
    long long zm_acc_re = 0, zm_acc_im = 0;  // ZM平均累加器
    uint16_t zm_avg_cnt = 0;       // 已累加次数
    uint16_t dnb_bad_count = 0;     // DNB 连续无响应计数 (自愈触发)
    uint16_t zm_conv_target = 100;  // 本次 ZM 的转换时间门 (按频率 exp 自适应, ZM 启动时算)
    uint8_t  bal_running = 0;       // 均衡进行中
    uint8_t  last_sweep_req = 0;    // 扫频线圈电平上次值 (边沿检测)
#define DNB_BAD_LIMIT  3           // 连续3次(~1.5s)温压全0 = DNB掉枚举, 触发自动重枚举
#define ZM_CONV_CYCLES  100       // ZM 转换时间门: 等够这么多轮询周期就读结果(原厂按 ConvTime, 不死等 BalZMDone). 100周期~6-8s, 安全超过原厂~4-5s转换时间

#define MEASURE_INTERVAL  50000   // ~500ms @ 8MHz

    while (1) {
        uint8_t c;

        /* USART1 (J12/蓝牙) Modbus 帧组装 — 与 USART2 独立, 同样空闲间隙断帧。 */
        if (usart1_rx_pop(&c)) {
            do {
                if (frame_idx1 < sizeof(frame_buf1)) frame_buf1[frame_idx1++] = c;
            } while (usart1_rx_pop(&c));
            idle_ticks1 = 0;
        } else {
            idle_ticks1++;
            if (frame_idx1 >= 8 && idle_ticks1 > 5000) {
                g_reply_port = 1;
                process_modbus(frame_buf1, frame_idx1);
                frame_idx1 = 0;
            }
        }

        if (usart2_rx_pop(&c)) {
            /* Drain everything the ISR has buffered so far in one go. */
            do {
                if (frame_idx < sizeof(frame_buf)) frame_buf[frame_idx++] = c;
            } while (usart2_rx_pop(&c));
            idle_ticks = 0;
        } else {
            idle_ticks++;
            measure_ticks++;

            if (frame_idx >= 8 && idle_ticks > 5000) {
                g_reply_port = 2;
                process_modbus(frame_buf, frame_idx);
                frame_idx = 0;
            }

            // DNB1101 定期测量: 从测量芯片 U8(ID=2) 读温度+电压。命令间留 ~4ms 间隔。
            if (dnb_ic_count > 0 && measure_ticks >= MEASURE_INTERVAL) {
                measure_ticks = 0;

                // ⚠️ ZM 进行中绝不读 VM/温度: GetData(MainVolt) 会打断正在进行的阻抗测量
                // (VM 与 ZM 共用 ADC), 导致 BALZMDONE 永不置位、测量永不完成。原厂 ZM 期间只轮询 SRVREQ。
                if (!zm_running) {
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

                // ── DNB 自愈: 温度+电压原始值同时为0 = DNB掉枚举(电池/夹子接触瞬断). ──
                // 连续 DNB_BAD_LIMIT 次判定掉线 → 自动重跑枚举+Init+阈值(与boot同序列), 接触恢复后自愈, 不用手动复位.
                if (jcy_dnb_temp_raw == 0 && jcy_dnb_volt_raw == 0) {
                    if (++dnb_bad_count >= DNB_BAD_LIMIT) {
                        dnb_bad_count = 0;
                        jcy_reenum_count++;
                        dnb_ic_count = dnb_enumerate();
                        if (dnb_ic_count == 0) dnb_ic_count = DNB_CHAIN_LEN;
                        dnb_init_ic(1, DNB_CHAIN_LEN);
                        dnb_delay_cycles(8000);
                        dnb_init_ic(DNB_MEAS_ID, DNB_CHAIN_LEN);
                        dnb_delay_cycles(8000);
                        dnb_set_thresholds();
                        dnb_delay_cycles(8000);
                    }
                } else {
                    dnb_bad_count = 0;
                }
                }   // end if(!zm_running) — ZM 进行中跳过 VM/温度读取

                // ── 扫频控制 (coil 0x00C0): 上升沿=启动整段扫频, 下降沿=中止 ──
                if (sweep_start_req && !last_sweep_req && !sweep_running && jcy_sweep_count > 0) {
                    sweep_running   = 1;
                    jcy_sweep_state = 1;            // 跑中
                    jcy_sweep_idx   = 0;
                    if (jcy_autorange) {            // 自动挡: 先用 10Ω 在中低频探测点定档, 再正式扫
                        jcy_samp_res = 0;            // 探测用 10Ω(最温和)
                        autorange_probing = 1;
                        jcy_zm_freq_set = AUTORANGE_PROBE_FREQ;
                    } else {
                        jcy_zm_freq_set = jcy_sweep_freq[0];
                    }
                    zm_start_req    = 1;            // 触发(探测点 或 第0点)
                } else if (!sweep_start_req && last_sweep_req && sweep_running) {
                    sweep_running   = 0;
                    jcy_sweep_state = 3;            // 中止
                }
                last_sweep_req = sweep_start_req;

                // 阻抗测量 (ZM): FC05 线圈触发 → 启动 → 轮询 BalZMDone → 读 Zreal/Zimag/VZM
                if (zm_start_req) {
                    zm_start_req = 0;
                    jcy_zm_done = 0;
                    jcy_status = 0x0001;        // ZM 测量中
                    dnb_delay_cycles(8000);
                    dnb_start_zm();
                    zm_running = 1;
                    zm_cycles  = 0;
                    zm_acc_re = 0; zm_acc_im = 0; zm_avg_cnt = 0;   // 重置平均
                    // 转换门按频率 exp 自适应: ConvTime=max(1100, 1050*2^(7-exp)) ms (原厂公式)。
                    // 低频(exp小)转换久(到~67s), 必须等够否则读到未完成的值。~40ms/cycle 保守(实测~55, 偏长安全)。
                    {
                        uint8_t ze = (uint8_t)((jcy_zm_freq_set >> 8) & 0x0F);
                        // conv_target 跟踪实际 ConvTime(轮询周期实测~225ms). 标准:~2x余量; 快速:~1.3x余量(省一半时间).
                        uint32_t conv_ms = (ze >= 7) ? 1100u : (1050u << (7 - ze));
                        uint32_t div  = jcy_zm_fast ? 180u : 110u;
                        uint32_t base = jcy_zm_fast ? 3u : 6u;
                        uint32_t tc = conv_ms / div + base;
                        zm_conv_target = (tc > 9000u) ? 9000u : (uint16_t)tc;
                        if (jcy_zm_convovr) zm_conv_target = jcy_zm_convovr;   // 调试覆盖(标定最低周期)
                    }
                } else if (zm_running) {
                    dnb_delay_cycles(8000);
                    uint32_t ss = dnb_get_status(DNB_MEAS_ID, DNB_STATUS_SRVREQ);
                    jcy_dnb_ss_hi = (uint16_t)(ss >> 16); jcy_dnb_ss_lo = (uint16_t)ss;
                    uint16_t flags = (uint16_t)((ss >> 4) & 0xFFFF);
                    // 原厂 MeasThread.c 逻辑: 不死等 BalZMDone(本配置下该位不置位), 而是等转换时间(ZM_CONV_CYCLES)
                    // 到了就读 Zreal/Zimag/VZM; BalZMDone 若提前置位也立即读。只要无 ZM/VM ADC 错误即认有效。
                    if ((flags & DNB_SRVREQ_BALZMDONE) || (++zm_cycles >= zm_conv_target)) {
                        if (flags & (DNB_SRVREQ_ZMADCERR | DNB_SRVREQ_VMADCERR)) {   // ADC 错误 → 无有效阻抗
                            dnb_delay_cycles(8000);
                            dnb_stop_zm();
                            zm_running = 0;
                            jcy_status = 0x0005;
                            sweep_on_point_done(0, 0, jcy_zm_vzm);  // 扫频: 本点ADC错→记0, 继续下一点
                        } else {
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
                                zm_acc_re += zr; zm_acc_im += zi;   // 累加(平均)
                            }
                            zm_avg_cnt++;
                            {
                                uint16_t navg = (jcy_zm_avg < 1) ? 1 : (jcy_zm_avg > 64 ? 64 : jcy_zm_avg);
                                if (zm_avg_cnt < navg) {            // 还没够 → 再测一次累加
                                    dnb_delay_cycles(8000); dnb_stop_zm();
                                    dnb_delay_cycles(8000); dnb_start_zm(); zm_cycles = 0;
                                } else {                            // 够了 → 输出平均
                                    jcy_zm_re64 = zm_acc_re / navg;
                                    jcy_zm_im64 = zm_acc_im / navg;
                                    jcy_zm_done = 1;
                                    jcy_status  = 0x0006;   // 测量完成
                                    dnb_delay_cycles(8000);
                                    dnb_stop_zm();          // 关 EnZM, 恢复 VM
                                    zm_running = 0;
                                    sweep_on_point_done(jcy_zm_re64, jcy_zm_im64, jcy_zm_vzm);  // 扫频: 存本点→下一点
                                }
                            }
                        }
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
