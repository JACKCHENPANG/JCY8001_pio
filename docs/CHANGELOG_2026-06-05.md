# JCY8001 改动存档 — 2026-06-05

单通道 EIS 阻抗测试（DNB1101 / STM32F103RC / JDY-10 蓝牙 / Web 蓝牙上位机 jcytest.com）一轮集中改进。

---

## 一、设备唯一序列号（不用手机号绑定）

- **固件 v2.23**：把 STM32 出厂 96 位 UID（地址 `0x1FFFF7E8`）暴露成 Modbus 寄存器 `0x3E10–0x3E15`。
- **网页 v1.2.0**：连上自动读 UID → CRC32 → `JCY-XXXXXXXX`（确定性、每台唯一；53 台架板 = `JCY-85CDEB48`）。每条测试记录绑 `device_serial`；历史按序列号查。原"手机号"改为"操作员(选填)"。
- **后端**：`battery_impedance_tests` 加 `device_serial` 列 + 索引；`/list`、`/save` 支持序列号。

## 二、测试数据管理后台

- `https://www.jcytest.com/admin/`，账号 `admin` / 密码 `jcy-admin-2026`（后端 env `JCY_ADMIN_USER`/`JCY_ADMIN_PASS` 可改）。
- 功能：统计卡（总记录/设备数/电池码数/近7天）、记录表（按序列号/电池码/操作员/日期筛选+分页）、行详情（Nyquist 谱图 + 照片 + 全参数）、CSV 导出（带 BOM 中文表头）。
- 后端 `/admin/login`（账密→12h session token）+ `/admin/list|record|stats|export`，走 `X-Admin-Token`，与产线 Bearer 分离。

## 三、SOC / 温度归一化（替代物理均衡）

- 软件归一化，瞬时、不拖产能。**Rs 与 SOC 无关（最鲁棒分选键）；Rct 随 SOC 非线性变需修正。**
- `detectChemistry(ocv)`：按 OCV 分桶 LFP(2.9–3.45V)/三元·钴酸锂(3.45–4.3V)/钛酸锂(2.0–2.9V)。
- `socFromOcv`（文献 OCV-SOC 表，LFP 平台段标"不可靠"）+ `kSoc`（文献 Rct(SOC) 相对曲线，参考 50%）+ `kTemp`（Arrhenius 归一到 25℃）。
- `normalizeRct(rct,ocv,temp)` → Rct 归一到 50%SOC/25℃ + 置信度。曲线均为**默认文献值，待标定**；原始谱全存可重算。
- **混料根本约束**：跨型号/容量/化学直接比内阻物理不成立 → 先按化学/型号分组，组内才比；分选用相对排序。
- 后续：数据自学习（同型号攒量自动拟合 k(SOC)）；用户做完充放电后金标标定。

## 四、温度探头（电芯温 vs 芯片温）

- 现 `0x3300` 是 DNB1101 芯片 die 温（GetData `0x04 MainDieTemp`），非电芯温。
- DNB110xB 数据手册 6.12：**不支持外接 NTC**，用内部双 DTS + 自发热补偿（需芯片贴电芯，本机经变压器+测试夹连接，不适用）。
- 方案：测试夹固定一个 **DS18B20** 测环境温（电芯静置=电芯温），接 STM32 空闲 GPIO，新寄存器 `0x3301`。固件 v2.24 已加读取框架（引脚待 Altium 定 + DWT 不可用需改 SysTick µs 延时，暂停 `ds18b20_poll`）。上位机 kT 优先用电芯温(0x3301)否则回退芯片温。

## 五、扫频/分选 速度与质量优化（重点）

### 问题与排查
- 用户报：高阻三元电芯测不好、几分钟不完成、计时不显示。
- **计时 bug**（v1.3.2）：单芯页 onTick 把秒数写进隐藏 dataset → 改成实时秒表。
- **测不好**：极速档只扫到 7.6Hz，高阻三元 Rct 弧峰在 ~0.24Hz 以下、弧没闭合 → Rct 不准（圆/ECM 差一倍）。
- **死循环重测**（v1.3.5）：质量门要求圆拟合出 Rct，但高阻三元有 Warburg 尾圆拟合必失败 → 无限重测。修：圆拟合失败用 ECM 兜底、容许掉 1 点、低频不预热。
- **avg 白等**（v1.3.4）：低频档 avg=4 比 avg=1 多采 3 倍，精度靠扫到弧顶不靠多采 → 全调 avg=1。

### 关键发现（速度真因）
- **真因不是蓝牙、不是 MCU 主频，是 DNB 每点转换时间**：每点 ~3.3s（ConvTime ~1.4s + DNB + 读数），11 点 ≈ 38s，**有线/蓝牙都一样**。
- 降 ConvTime（实验 v2.28，450ms）→ 内阻测错 + 反而更慢 → 回退。ConvTime ~1.1s 是测准必需，砍不得。
- （插曲：曾误读"有线 2.7s"，实为 stale-read 缓存假象。）

### 最终方案
- **自动档已下线**，三档：分选快测(默认) / 极速谱 / 精测Rct。
- **分选快测**（v1.4.2）：8 点 `[0,2,5,8,12,14,15,16]`（1kHz→0.95Hz）~30s。不闭合弧、不拟合 Rct，排序键 = **Rs + 低频 |Z|**（温度归一）。`measureGood` 加 sort 旁路（不卡 Rct/RMS 重测）。
- **精测Rct**：扫到 0.12Hz 闭合弧出准确 Rct（2-3 分钟）。
- **记住挡位**（v1.4.1）：自动挡首颗探档后锁定，后续复用跳过 ~14s 探档；"🔒已锁挡 · 重新定档"。
- 后端加 `mode`/`zlow`/`zlow_norm` 列 + admin/CSV。

### 实测（53 台架，同颗三元）
- 分选 8 点：~30s 稳定；**排序键 |Z|@0.95Hz CV 0.03%**；Rs CV 2.45%。
- 点数 vs 时间近线性：11pt≈38s / 8pt≈30s / 6pt≈23s / 4pt≈17s。Rs/Rct 圆拟合 ≤6 点失败（HF 点不够）。

## 六、固件主频（B）

- **发现板上 8MHz HSE 晶振不起振**（原厂"72MHz"配置在本板从没跑起来，一直 8MHz）。
- 改用内部 **HSI/2 × PLL16 = 64MHz**（不依赖晶振，8 倍速）。
- 所有时钟相关常数动态化：USART BRR（`g_pclk1/2`）、SPI 预分频（`g_clkmul`）、`dnb_delay_cycles`（×`g_clkmul`）、`idle_ticks` 帧间隔、`MEASURE_INTERVAL`、SysTick LOAD。PLL 失败保底 8MHz（动态 BRR 仍通信）。
- 收益：数据更干净（少抢蓝牙、少掉点）、更一致；测量时间因 DNB-bound 未显著变。
- **DWT CYCCNT 在本芯片不计数**（疑国产 clone）→ 时间门基准从 DWT 改 SysTick（`g_ms` 1ms tick）。

## 七、版本落点

- **固件**：v2.27（64MHz / SysTick 时间门 / UID 寄存器 / 动态时钟）。调试寄存器 `0x3E32`=g_ms、`0x3E33`=主频MHz。已烧 53。
- **网页**：v1.4.2（分选快测 8 点 / 锁挡 / SOC 归一化 / 实时计时 / 电芯温优先）。已上线 jcytest.com（已加 nginx no-cache 防旧版缓存）。
- **后端**：device_serial / chemistry / soc / rct_norm / mode / zlow 列；admin 后台。

## 待办
1. DS18B20 电芯温探头：Altium 原理图定空闲 GPIO + SysTick µs 延时 + 接探头联调。
2. SOC 归一化曲线标定（充放电后金标电芯）+ 数据自学习。
3. 是否把这套移植到 JCY5001（8 通道，原厂 NDB110x 多通道基线 = `~/Projects/hardware/NDB11xx_code`，菊花链最多9颗 DNB + 595 继电器 + 串口屏 + 均衡）。
