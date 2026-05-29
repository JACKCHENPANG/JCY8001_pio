# JCY8001 DNB1101 软件侧调试记录 — 2026-05-29

承接 `BUILD_RECORD_2026-05-28_DNB_SMOKE.md`。目标：DNB1101 温度(0x3300)/电压(0x3340) 一直读 0，定位原因。

## 环境

- 固件源码：`/Users/jcy/Projects/hardware/JCY8001/JCY8001_pio`，`src/main.c`
- 编译：本地 PlatformIO 6.1.19，`genericSTM32F103RC`，SUCCESS，Flash 2392 B / RAM 336 B
- 烧录主机：`ubuntu@192.168.0.53`，J-Link V9.5（`/usr/bin/JLinkExe`）+ CP2102（`/dev/ttyUSB0`）
- 烧录脚本：`/tmp/flash.jlink`（si SWD / device STM32F103RC / erase / loadfile /tmp/firmware.bin 0x08000000 / w4 0x08004000 0x12345678 / r / g）
- 验证：`tools/jcy8001_smoke.py --port /dev/ttyUSB0`（Modbus RTU 115200 8N1 addr=1）

## 重要纠错：固件实际用哪套 SPI

- **`src/main.c` 自带裸寄存器 SPI/DNB 驱动**，只 `#include "stm32f1xx.h"`。
- `lib/spi/spi.c`、`lib/dnb11xx/*` **未被编译**（LDF chain 模式，build 里仅 `src/main.o` + HAL）。这两个目录是死代码。
- 因此「把 `lib/spi/spi.c` 的 MISO 从 AF_PP 改成浮空输入」是 **空操作**，已回退。
- `main.c` 真实 GPIO 配置（`spi1_init` 第119行）：
  ```c
  GPIOA->CRL = (GPIOA->CRL & 0x0000FFFF) | 0xB4B00000;
  // PA7(MOSI)=0xB AF-PP out, PA6(MISO)=0x4 浮空输入(正确), PA5(SCK)=0xB AF-PP out
  GPIOB->CRL = (GPIOB->CRL & 0xFFFFF0FF) | 0x00000300; // PB2(NSS)=0x3 推挽输出
  SPI1->CR1  = 0x0354; // master, mode0(CPOL0/CPHA0), MSB, 8bit, SSM/SSI, BR=/8 → 1MHz
  ```
  PA6 已是浮空输入 = STM32F1 master 输入脚正确配置。**MCU 侧没有 GPIO bug。**
- 5-28 记录里 dnb_dbg 尾部 `0xFFFF/0xF0FF` → 本次 `0x0000` 的变化，是当前工作区 main.c 与 5-28 烧录的 bin（ead25832）本身不同 + 重新 erase 所致，**与任何 GPIO 改动无关**。

## SPI Mode 扫描（CR1 = 0x0354/0355/0356/0357）

逐个改 `SPI1->CR1`，编译→烧录→读寄存器：

| CR1 | mode | temp 3300 | volt 3340 | status 3380 | dnb_dbg 3E20 |
|------|------|-----------|-----------|-------------|--------------|
| 0x0354 | 0 (CPOL0/CPHA0) | 0 | 0 | 0x0003 | 00FF 0000 0000 0000 0000 0000 0000 |
| 0x0355 | 1 (CPHA1) | 0 | 0 | 0x0003 | 00FF 0000 0000 0000 0000 0000 0000 |
| 0x0356 | 2 (CPOL1) | 0 | 0 | 0x0003 | 00FF 0000 0000 0000 0000 0000 0000 |
| 0x0357 | 3 (both) | 0 | 0 | 0x0003 | 00FF 0000 0000 0000 0000 0000 0000 |

**四种 SPI mode 结果完全一致，原始 RX 不变。** SPI 时序模式不是变量。
板子已回烧 mode0 基线（sha256 `fd1baaa6c6ee63fc11cf34c9068244e5c1033db65c87af76f8309d6f52b6ee19`）。

`dnb_dbg 3E20` word0 = `0x00FF` 即 `rx[0]=0x00, rx[1]=0xFF`，其余全 0，恒定。

## 软件层分析（main.c）

1. **菊花链延迟读（头号软件嫌疑）**：DNB11xx 是移位链，命令发进去后响应在「下一帧」才移出。`dnb_send_cmd`（main.c:209）单帧发完即读 rx，再用 a/b 双窗口去猜。这与参考驱动里 Init/SetMode「双发」(main.c:258-260, 267-269) 的事实吻合——双发就是为了第二帧把 ACK clock 出来。`dnb_get_data` 是单发，所以读不到数据。
   - 但：SPI mode 扫描显示 MISO 无论如何都不带有效数据，说明即使把读时序改对，若 DNB 没在 clock 时驱动 MISO，依旧是 0。延迟读修正需在「确认 DNB 确实在驱动 MISO」之后才有意义。
2. **enumerate 误判**：`dnb_enumerate`（main.c:242）用魔数探针 `{0x52,0x00,0x01,0xFF}`，读 `rx[3]`。MISO 数据混杂时易误判 ic_count=1（ch_count 3E00=1 即来源于此），导致后续 Init/SetMode/测量照跑但全空。

## 结论与分工

- MCU SPI 配置（GPIO/mode）已排除，不是问题根因。
- 现象（4 种 mode 下 MISO 恒为 `00FF...`、无随时钟变化的有效位）强烈指向 **DNB1101 侧没有有效响应**：
  - DNB 需电池单体供电（main.c:378 注释「需电池供电」）。若工装上 DNB 未接电芯，必然无响应。
  - 或 MISO 物理链路 / DNB 上电复位 / 唤醒未到位。
- 软件侧在无硬件确认前已基本到顶。

## 下午逻辑分析仪测试清单（必做）

1. 抓 **SCK / MOSI / MISO / NSS(PB2)** 四路。
2. 确认 NSS 在每帧拉低、SCK 正常 1MHz、MOSI 上能看到 `0F xx xx xx CRC ...` 命令帧。
3. 重点看 **MISO 在 SCK 有效沿时 DNB 是否真的在驱动**：
   - 若 MISO 始终跟随空闲电平（无随 SCK 翻转的数据位）→ DNB 未响应 → 查 **DNB 供电（电芯）/ 复位 / 片选 / 唤醒**，而非固件。
   - 若 MISO 有数据但出现在命令帧的**下一帧** → 证实菊花链延迟读，回来改 `dnb_send_cmd`：发命令帧后再发一帧 dummy，从第二帧解 ACK/数据。
4. 顺带量 DNB 供电脚电压、确认电芯接入。

## 追加：原厂固件 + 外供电验证（同日，后续）

用户接入外供电后，烧入原厂工作固件验证硬件：

- 原厂固件：`firmware/JCY8001_combined.hex`（bootloader@0x08000000 + NDB110x_V1.2 app@0x08005000，合并烧录），md5 `69e7475807d17367765ac027845559af`。J-Link `loadfile /tmp/combined.hex` O.K.
- 寄存器表来源：`JCY8001_bak/docs/JCY8001_Modbus_Protocol.md`（v2）。温度 0x3300（16位,1位小数,℃）、电压 0x3340（16位,4位小数,V），与 smoke 工具地址一致。
- 读数（Modbus FC04）：
  | 寄存器 | 原始 | 解码 |
  |--------|------|------|
  | status 0x3380 | 0x0000 | 空闲（**之前 dev 固件读到 0x0003 = 电池电压错误/未安装**）|
  | temp 0x3300 | 0x0163 (355) | **35.5 ℃** |
  | volt 0x3340 | 0xEA60 (60000) | **6.0000 V**（对上 6V 外供电）|

**结论：硬件 + DNB1101 完全正常。** 原厂固件能正确读温度/电压。

→ 问题确定在 **dev 固件 src/main.c 的 DNB SPI 读协议**（菊花链延迟读 / 单帧读 bug），不是硬件、不是 GPIO、不是 SPI mode。修 dev 固件时参考原厂 Keil 驱动 `Driver_DNB11xx.c`（菊花链响应延迟一帧），把 `dnb_send_cmd` 改成发命令帧后再发一帧 dummy、从第二帧解响应。
之前 dev 固件读到 status=0x0003，也部分因当时无外供电/电池。

## 追加二：按原厂协议重写 dev 固件 DNB 读取层（方案A）

照原厂 `firmware/Sources/Driver_DNB11xx.c` 把 `src/main.c` 的 DNB 层重写：

- **CRC4**：`crc4_lookup_fast` 查表 (poly x^3+x^0, 表 `{00,09,0B,02,0F,06,04,0D,07,0E,0C,05,08,01,03,0A}`)，覆盖 ulData nibble B7..B1（跳过 B0=CRC）。
- **命令字**：`ulData = [ID:8(b24-31)][CMD:4(b20-23)][Data:16(b4-19)][CRC4:4(b0-3)]`。
- **帧**：`[head×0x00][0x0F][ID][CMD|DataHi][DataMid][DataLo|CRC][ics*4×0xFF][0xF0]`，普通命令 head=8，上电首帧 head=430（flush 菊花链）。
- **双发**：菊花链响应延迟一帧 → 命令发两次（隔~8ms），响应读 `RecvBuf[head+4+(ID-1)*4]` 起 4 字节（big-endian ulData）。
- **枚举/Init/SetMode**：GetStatus CheckID → 未枚举则 Enum(SetID=1)；Init Data=0x1001（NrOfICs=1,EnSrvReq）；SetMode Normal=4。
- **转换**（对齐原厂，寄存器标度）：
  - 电压 0x3340 = V×10000，V=MainVolt(14bit,b4-17)/16383×4.8+1.2 → `f*48000/16383+12000`
  - 温度 0x3300 = T×10，T=MainDieTemp(12bit,b4-15,有符号)×0.0625 → `t12*5/8`
- 调试：加 `0x3F00+n → dnb_rx[2n..2n+1]` 原始 RX 窗口 peek。

编译 SUCCESS（RAM 1360B，含 1KB dnb 缓冲；Flash 2480B）。

**实机结果：仍读不到。** 多轮实验：

1. 初版（带 NSS toggle）：`dnb_rx` 整帧全 **0x00**，enumerate 返回 0。
2. 去掉 spi1_full_duplex 里的 PB2/NSS toggle（原厂用 soft-NSS，传输期间不驱动 CS）：`dnb_rx` 整帧变 **0xFF**（idle high）。
3. ⚠️ **假阳性陷阱（务必记住）**：去 NSS 后 volt 寄存器读出 0xEA60=6.0000V、vraw=0x3FFF，一度以为电压通了。实为 **MISO idle 0xFF 的算法假象**：响应窗口全 0xFF → `(0xFFFF>>4)&0x3FFF = 0x3FFF = 16383 = 满量程`，按公式正好 = 6.0000V（外供电恰好 6V=满量程）。温度同理 0xFFFF→t12=-1→0℃。**dump 整帧 RX 全 0xFF（unique bytes=[0xFF]），芯片根本没驱动 MISO。** 切勿用 0x3FFF/满量程值判定“读通”。

结论：dev 固件下 MISO 始终是 idle（带 toggle=0x00 / 不带=0xFF），**芯片对 dev 的命令零响应**；而原厂固件在同板同供电下读出 34.7℃/6.0000V（temp=0x15B 是具体值非满量程，确属真实测量）。所以差异在 dev 与原厂的物理层/时序：

- 系统时钟：dev 8MHz HSI（SPI~1MHz）vs 原厂 72MHz HSE+PLL（SPI~1.125MHz）。
- SPI：dev 轮询逐字节（字节间 SCK 有间隙）vs 原厂 **DMA 连续时钟**。← 头号嫌疑：菊花链 cell-monitor 可能要求帧内 SCK 连续无间隙。
- 完整初始化序列（SetThVolt/SetThTemp）未跑。

## 追加三：突破 —— 无间隙时钟，芯片响应，温度读通 ✅

按上面"DMA 连续时钟"猜想动手，**确为根因**：

1. **去 NSS toggle**（原厂 soft-NSS，传输期间不驱动 PB2）。
2. **无间隙流水线 SPI**：`spi1_full_duplex` 改为「一有 TXE 立刻预填下一字节」，帧内 SCK 连续无间隙（等效原厂 DMA）。dev 原来逐字节轮询、字节间 SCK 有间隙 → 菊花链 cell-monitor 不响应。
3. **命令间加 ~4ms 间隔**（镜像原厂 MeasThread 各读之间的调度间隙），多读才都响应。

改完芯片真响应了：**温度读通，36.2℃，traw=0x244(580)*0.0625，与原厂 34.7℃ 一致，是具体值非满量程，确为真实数据。** GeneralStatus 也返回真值 0x018010C0。

### 仍未解决：MainVolt = 0
- 温度对、电压 MainVolt 字段恒为 0（volt 响应 `01 80 00 0B`，温度响应 `01 80 24 48`，同结构同 ACK=8，仅数据字段差）。
- 同板同时刻原厂固件读 volt=6.0000V，dev 读 0 → **是 dev bug，非电芯输入**。
- `dbg=0xE0F` → **dev 的 Init 和 SetMode ACK 校验都失败**。温度因 die-temp 常开仍可读；电压 VM 因 SetMode/Init 未真正生效而不跑 → MainVolt=0。
- 加 SetThVolt(0xFF00)/SetThTemp(0x7F80) 无效（只是阈值，不启 VM）。
- skip 全部 init → 温度也变 0，证明 dev 的 enumerate/Init/SetMode 是温度能读的前提。

## 追加四：完全解决 ✅✅ —— 电压来自 ID=2 测量芯片

查数据手册 `docs/DNB110xB_数据手册_V2.0.pdf` + BOM + 网表，定位真因：

**JCY8001 板上有两颗 DNB1101**（BOM: `U6=SPI桥接, U8=测量`）：
- **U6 网关**：SPI_En(pin9)→`+3.3V` 硬接高 = 永久 SPI 模式 = SPI↔DIO 桥接。数据手册表42：SPI 模式下**只有 TM(温度)，VM(电压)/ZM 关闭**。
- **U8 测量**：SPI_En→`VBAT1_VM-`(低) = 测量模式；VBAT→`VM1`(电芯 6V)。VM/TM/ZM 全开。
- 拓扑：MCU SPI → U6(网关) →差分DIO→ U8(测量)。

dev 之前一直读 **ID=1=U6 网关** → 温度有(TM 在 SPI 模式可用)、电压 0(VM 在 SPI 模式关闭)，完全对上！**电芯电压在 U8=ID=2。**

**最终修复（4 处叠加）**：
1. 无间隙流水线 SPI 时钟（帧内 SCK 连续，等效原厂 DMA）。
2. 不 toggle PB2/NSS（原厂 soft-NSS）。
3. 命令间 ~4ms 间隔。
4. **从 ID=2(U8 测量芯片) 读 MainVolt/MainDieTemp，链长 ics=2**（响应偏移 head+4+(2-1)*4=16）。

实测稳定：**温度 34.7~35.0℃、电压 6.0000V，与原厂固件完全一致。dev 固件全通。**
转换：电压 0x3340=V×10000，V=MainVolt/16383×4.8+1.2 → `f*48000/16383+12000`；温度 0x3300=T×10，T=MainDieTemp(有符号12bit)×0.0625 → `t12*5/8`。

板上当前为 **dev 固件（温度+电压均正确）**。

## 追加五：清理 + 冷启动枚举 ✅

- **清理**：删掉诊断 dump 缓冲(voltdump/tempdump)、0x3F00/0x3F40 RX peek、SetMode(Standby/Normal)实验代码、SetThVolt/SetThTemp、未用的 measure_phase。编译无警告，Flash 2312B。
- **冷启动枚举**：`dnb_enumerate()` 改为遍历 `DNB_CHAIN_LEN=2`，逐个 GetStatus CheckID；若 ID 不符则 `Enum(ID=0, SetID=target)` 分配。暖启动(ID 已存在)时只确认不重发 Enum(非破坏)。Init 两颗 IC(链长=2)。读测量值固定从 `DNB_MEAS_ID=2`(U8)。
- **实测(暖启动)**：`enum_found=2`(两颗 IC 都确认到)，温度 34.7℃ / 电压 6.0000V 稳定。
- ⚠️ **冷启动(真正断电再上电)枚举路径无法远程验证**(无法远程给 DNB 断电)。逻辑照原厂 Enum 握手实现，需在工装上断电重上电验证一次。

**清理后状态：板上 dev 固件温度+电压均正确，代码已精简。** main.c 关键: `DNB_CHAIN_LEN=2`, `DNB_MEAS_ID=2`, 无间隙流水线 SPI, 不 toggle PB2, 命令间 ~4ms。

## 追加六：通讯指令审查修复 + 阻抗测量(ZM/EIS)移植

### Modbus 通讯审查 + 修复
- 🔴 FC01/02/03/04 `count` 无上限 → `tx_buf[256]` 溢出。已加校验: FC03/04 count>125、FC01/02 count>2000 → 回异常 0x03。
- 🟡 不支持的功能码原来无响应 → 已加 `default` 回异常 0x01 (实测 FC05 在未启用 ZM 路由前回 `01 85 01`)。
- 🟡 FC01 线圈位恒置 bit0 (只对 coil0 正确, 已知局限, 未改)。

### 阻抗测量移植
- FC05 写线圈 0x0000=FF00 → 启动 ZM; 0000 → 停。
- `dnb_start_zm()`: SetZMFreq(0x07, 频率=jcy_zm_freq_set) + SetZMCurr(0x06, EnZM=bit11) 发到 U8(ID=2)。
- 测量循环轮询 SrvReq 的 BalZMDone(bit7); 完成则读 Zreal(0x07)/Zimag(0x08)/VZM(0x06), 取 `ulData>>4 & 0xFFFF`(ZMantissa|ZExp<<12 原始值, 主机侧换算 Ω, 原厂 cal_ZMR 也是注释掉留给主机)。
- **超时保护**: 12 周期(~6s)未完成 → `dnb_stop_zm()`(SetZMCurr EnZM=0) 关 ZM 恢复 VM, status=0x0005。修掉了"ZM 卡住把电压压成 0 直到复位"的坑。
- Modbus 寄存器: RE 0x3000 / IM 0x3080 / VZM 0x3200 (各取低 16 位原始) / ZM完成标志 0x3E2D / 频率设置 0x4200 (FRQMantissa|FRQExp<<8|LFNS<<12, 默认 0x0A09≈68.6Hz)。

### 实测(6V 台式电源, 非真实电芯)
- FC05 启动 → status=0x1, 芯片进 ZM, SrvReq=0x0044 = VM-ADCErr(bit2)+CurrErr(bit6) → **无真实电芯无法测阻抗, BalZMDone 不置位**(符合预期)。
- ~6s 超时 → 自动关 ZM, **volt 恢复 6.0000V**, 不再卡死。
- ⚠️ **阻抗 RE/IM 数值无法用台式电源验证, 需接真实电芯 + 参考仪器在工装上确认。** 命令路径/超时/恢复均已验证 OK。

main.c Flash 2908B, 编译零警告。温度+电压仍正常。

## 追加七：FC0F/FC10 + 均衡 + 参数寄存器

- **FC0F (写多线圈) / FC10 (写多寄存器)**：补齐, 带 qty/byteCount 校验, 越界回异常 0x03。
- **参数寄存器** (主机 FC03/06/10 读写, 各取首通道): 采样电阻 0x40C0(0~3), 均衡电压 0x4100(0~255), 均衡时间 0x4140(0~255), PWM 0x4180(0~14), ZM 增益 0x4280(1/4/16)。默认: gain=1, balVolt=133。
- **均衡 (Balance)**: 线圈 0x0040=启动/停止, 0x0080=模式(0时间/1电压)。`dnb_start_balance()`= SetBalVolt(09h, BalMode+目标电压) + SetBalCurr(08h, EnBal=1, PWM, 超时) 发 U8; `dnb_stop_balance()`= SetBalCurr EnBal=0。主机 balVolt 0~255 → 芯片 14位 ≈值*64 (近似, 待标定)。
- **实测**: FC06/FC10 写参数 + 读回正确; FC0F/FC05 线圈回显正确; 启动均衡时 **volt 保持 6.0000V**(均衡与 VM 共存, 不像 ZM 压电压); 温度+电压全程正常。
- ⚠️ 均衡实际效果(电芯放电)与阻抗数值同理, 需真实电芯验证。balVolt 的 0~255→14位 换算系数(*64)为近似, 需对标原厂/标定。

main.c Flash 3672B, 编译零警告。

## 追加八：群发地址 + 多通道

- **群发参数 (0x4F0x, FC06/10 写全部通道)**: 0x4F01 ZM平均, 0x4F03 采样电阻, 0x4F04 均衡电压, 0x4F05 均衡时间, 0x4F06 PWM, 0x4F07 频率, 0x4F09 ZM增益。本单通道板上群发=写本通道。
- **群控线圈 (0x0F0x, FC05/0F)**: 0x0F00 启全部 ZM, 0x0F01 启全部均衡, 0x0F02 均衡模式。
- **多通道地址**: 协议每寄存器是 64 通道范围(base+(n-1)); 本板物理只有 1 颗测量 IC(U8)=1 通道, 故 ch1(base) 返回真实数据, ch2~64 返回 0。
- **实测**: 群发 0x4F04=111 → 读回本通道 0x4100=111 ✓; 0x4F09=4 → 0x4280=4 ✓; 群控线圈 0x0F00=ON → status=0x1(ZM启动) ✓; ch1 0x3300=35.0℃、ch2 0x3301=0 ✓。

main.c Flash 3796B, 零警告。

## 追加九：ZM 测量模式 (低阻/低频模式) + 协议

- 新寄存器 **0x4300 ZM 测量模式**: 0=普通(用单独设的 gain/LFNS), 1=低阻低频(强制 LFNS=1 + 增益16)。群发 **0x4F0A**。
- `dnb_start_zm()` 现按 `jcy_zm_gain` 映射 HiPass(1→00,4→01,16→10); 模式=1 时强制 LFNS(freq bit12)+HiPass=16, 覆盖单独设置。
- 背景(EIS 低阻<1mΩ/低频<1Hz 随机抖动): 错频/分组对随机噪声无效; 治随机抖靠 LFNS+多平均(1/√N)+大电流+Kelvin 四线。低阻模式把 LFNS+增益16 一键打开, 平均/电流仍由主机配(平均建议主机多读取平均, 或后续加固件软平均)。
- 实测: 0x4300 写1读回1、群发 0x4F0A 写0读回0、ZM 仍正常启动、温度+电压正常。

main.c Flash 3916B, 零警告。

## 追加十：阻抗 μΩ 换算 + 64位输出 (对齐 LY8001 上位机)

**起因**: 上位机 LY8001(JCY5001-LAB) data_read_manager 读 RE 0x3000/IM 0x3080 各 count=32(8通道×4寄存器=64位大端有符号), value/100000=μΩ。原 dev 固件发 1个16位原始 M/E → 不兼容。

**移植**: 从原厂 `firmware/Threads/DNB11xx_ArrayDef.c` 把校准表+换算搬进 `lib/dnb_zm/dnb_zm.c`:
- 公式 `Z = mantissa*2^exp * Rext / (VZM * CoeffMapArr[idx])`, ZI 取负。
- CoeffMapArr(455 double 校准表) + MantMapArr/ExpMapArr(各455, 用于 M/E→频率索引反查)。
- VZM(mV) = vzm_raw * 4800/16383 + 1200; ResisList[4]={10,5,3.33333,2.5}(采样电阻档)。
- 频率索引: 由 0x4200 设的 M/E 反查 dnb_zm_index()。默认频率改为校准表真实项 0x0746=66.757Hz(M=70,E=7)。
- main.c: jcy_zm_re64/im64 (int64), get_reg 0x3000~3003 / 0x3080~3083 输出 64位大端。

**验证**: 主机侧单元测试 dnb_zm.c —— 索引反查(M70/E7→idx413)✓、换算公式不崩、符号/越界保护 ✓。实机: temp 35.2℃/volt 6.0000V/freq_set=0x0746 正常, RE/IM 现按 4寄存器返回(8字节, 6V无电芯下=0, 符合)。
⚠️ **阻抗实际数值仍需真实电芯端到端验证**(校准公式照搬原厂, 但 6V 电源无法触发有效 ZM)。Flash 11444B(校准表+软浮点占 ~7.5KB)。

**dev 固件 Modbus 协议层完整**: FC01-10 全 + 温度/电压/阻抗(μΩ 64位)/均衡/参数/群发/多通道/ZM模式, 与 LY8001 上位机协议兼容。待真实电芯验证: 阻抗数值、均衡效果、balVolt 标定、低阻模式降噪。

## ⚠️ 原厂固件烧录关键：app 有效标志

原厂 `combined.hex` 烧录后**必须**写 `w4 0x08004000 0x12345678`（bootloader 的 app 有效标志），否则 bootloader 不跳转到 app@0x08005000，板子完全不应答 Modbus。本次曾因漏写该标志导致原厂固件“变砖”假象，补写后恢复，读出 35.0℃/6.0000V。恢复脚本 `/tmp/flash_recover.jlink`（erase + loadfile combined.hex + w4 0x08004000 0x12345678 + r + g）。

**当前板上 = 原厂工作固件（已写标志，读 35.0℃/6.0000V）。** dev 固件源码已含完整协议端口，留待下午逻辑分析仪联调。

## 本次改动

- 无固件功能性改动留存。`lib/spi/spi.c` 的临时改动已回退。`src/main.c` 的 `SPI1->CR1` 已回到 `0x0354`。
- 板上固件 = mode0 基线（sha `fd1baaa6`）。
