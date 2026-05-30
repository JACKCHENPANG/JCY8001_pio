# JCY8001 EIS 工具

通过 Modbus RTU 驱动 dev 固件做阻抗谱扫描 + Nyquist + Rs。

## 用法

```bash
# 1. 扫频 (默认 /dev/ttyUSB0, 20点 1kHz→0.119Hz, 1Ω档, 快速模式)
python3 eis_sweep.py [/dev/ttyUSB0] [eis.csv]

# 2. 画 Nyquist + 求 Rs (虚部过零点)
python3 eis_nyquist.py eis.csv [nyquist.png]
```

依赖: `pip install pyserial matplotlib`

## 实测性能 (1Ω档, ~3.3V电芯, fast 模式)

- 全 20 点 ~1.7 分钟; 高/中频 ~3s/点, 0.1Hz ~21s/点
- Rs(虚部过零) ≈ 969 μΩ, 与原厂固件吻合

## 关键寄存器

| 寄存器 | 含义 |
|---|---|
| 0x4200 | ZM 频率 = Mantissa\|Exp<<8, Freq=0.0074506·Mant·2^Exp (须用频率表有效 M/E) |
| 0x40C0 | 采样电阻档 0=10Ω 1=5Ω 2=1Ω |
| 0x4340 | ZM 速度 0=标准 1=快速 |
| 0x4360 | 转换门周期覆盖 (0=按频率自动, 调试用) |
| 0x0000 | 线圈: 写1触发ZM, 写0停止 |
| 0x3000 / 0x3080 | RE / IM, 各4寄存器=64位大端, value/100000=μΩ (IM 已是 -Z_imag) |
| 0x3E2D | zm_done (1=测量完成); 触发后先等归0再等回1, 否则读残值 |

## 坑

- **触发后必须先等 zm_done 归 0 再等回 1**, 否则读到上一点残值。
- 频率必须是固件表里的有效 M/E, 否则换算返 0。
- 转换时间是芯片硬底(ConvTime=max(1100,1050·2^(7-exp))ms), 高频~2.5s 低频~21s, 省不掉。
