# JCY8001 Firmware (PlatformIO)

JCY8001 阻抗分析仪固件，移植自 jcy8001_firmware 到 PlatformIO 开发环境。

## 硬件平台

- **MCU**: STM32F103RCT6 (72MHz, 256KB Flash, 48KB RAM)
- **测量芯片**: DNB1101 × 2 (U6 通讯, U8 实际测量)
- **通讯**: USART2 (Modbus RTU, 115200 8N1)

## 功能特性

### 已实现

- [x] SPI1 DNB1101 驱动 (PA5/PA6/PA7, 1MHz)
- [x] DNB1101 版本/状态/电压/阻抗读取
- [x] DNB1101 测量启动命令
- [x] USART2 Modbus RTU 协议栈
- [x] Modbus 寄存器映射 (输入/保持/线圈)
- [x] 100ms 周期 DNB1101 数据轮询
- [x] HSE 时钟配置 (8MHz × 9 = 72MHz)
- [x] PlatformIO 构建系统

### 待验证

- [ ] 实际烧录到硬件
- [ ] DNB1101 SPI 通讯握手验证
- [ ] 阻抗测量数据正确性
- [ ] Modbus 读写功能实测

## 项目结构

```
JCY8001_pio/
├── platformio.ini      # PlatformIO 配置
├── inc/
│   ├── spi.h          # SPI1 + DNB1101 驱动
│   ├── register.h     # Modbus 寄存器定义
│   ├── modbus.h       # Modbus RTU 协议栈
│   ├── usart.h        # USART2 驱动
│   ├── crc16.h        # CRC16 计算
│   └── stm32f1xx.h    # STM32 寄存器定义
├── src/
│   ├── main.c         # 主程序 + 时钟初始化
│   ├── spi.c          # SPI1 + DNB1101 实现
│   ├── register.c     # 寄存器读写 + DNB1101 轮询
│   ├── modbus.c       # Modbus RTU 实现
│   ├── usart.c        # USART2 实现
│   └── crc16.c        # CRC16 实现
└── .pio/              # PlatformIO 构建输出
```

## 编译

```bash
pio run          # 编译
pio run -t upload  # 烧录 (需 J-Link)
pio run -t size    # 查看大小
```

## 固件大小

| 指标 | 使用 | 可用 | 占用 |
|------|------|------|------|
| RAM | 1096 B | 49152 B | 2.2% |
| Flash | 2736 B | 262144 B | 1.0% |

## Modbus 寄存器

### 输入寄存器 (FC04)

| 地址 | 名称 | 说明 |
|------|------|------|
| 0x3000 | Z_RE | 阻抗实部 (Q16.16) |
| 0x3080 | Z_IM | 阻抗虚部 (Q16.16) |
| 0x3300 | TEMP | 温度 (×0.1°C) |
| 0x3340 | VOLTAGE | 电压 (×0.0001V) |
| 0x3380 | STATUS | DNB1101 状态 |

### 保持寄存器 (FC03/06)

| 地址 | 名称 | 说明 |
|------|------|------|
| 0x4000 | ZM_FREQ | 测量频率 (Hz) |
| 0x4040 | ZM_AVG_COUNT | 平均次数 |
| 0x4F03 | SAMPLE_RES | 采样电阻选择 |

## DNB1101 SPI 协议

- 模式: CPOL=0, CPHA=0 (Mode 0)
- 时钟: ≤1MHz
- 命令: 'R' 读, 'W' 写, 'M' 测量
- 数据: 大端序, Q16.16 格式

## Git

```bash
git remote -v  # 查看远程仓库
git push       # 推送 (已配置 origin)
```

## 参考

- [jcy8001-spi-development skill](file:///Users/jcy/.hermes/profiles/hmike/skills/hardware/jcy8001-spi-development/SKILL.md)
