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

## 采样电阻校准协议 (v2.14, 上位机可标定)

每档采样电阻实际阻值可由上位机标定并存 Flash 持久化(掉电不丢):

| 寄存器 | 含义 | 读写 |
|---|---|---|
| 0x40D0 | 10Ω档实际阻值 (mΩ, 默认10000) | FC03读 / FC06写 |
| 0x40D1 | 5Ω档实际阻值 (mΩ, 默认5000) | FC03读 / FC06写 |
| 0x40D2 | 1Ω档实际阻值 (mΩ, 默认1000) | FC03读 / FC06写 |
| 线圈0x0010 | 写1 = 保存当前标定到Flash | FC05 |

**标定流程**: ①电桥量出某档采样电阻实际阻值(mΩ) ②FC06写对应寄存器 ③FC05写线圈0x0010存Flash ④掉电重启自动加载。阻抗换算用标定后的实际阻值(Rext), 消除电阻容差带来的档间误差。

**DNB自愈 (v2.14)**: 运行时温度+电压原始值连续3次(~1.5s)都为0判定DNB掉枚举, 自动重跑枚举(接触瞬断自恢复)。调试寄存器0x3E30=自愈重枚举次数。

## EIS 布线/接触补偿 (AN2001) — tools/eis_wiring_comp.py
测量值含引线感性/阻性耦合 + 接触电阻偏移(每频率恒定)。一阶修正:
`Re_comp=Re+Mre·f+Rpar`, `Im_comp=Im+Mim·f+Ipar` (μΩ, f单位Hz)。

```bash
# 1.校准: 接参考件(短路件/精密电阻/电化学工作站真值)扫频, 拟合4参数
python3 eis_wiring_comp.py cal ref.csv --re-true 0 --im-true 0 -o wiring.json   # 短路件
python3 eis_wiring_comp.py cal ref.csv --truth truth.csv -o wiring.json          # 电化学工作站逐频真值
# 2.应用: 补偿被测扫频
python3 eis_wiring_comp.py apply meas.csv wiring.json -o meas_comp.csv
```
真值来源: 短路件(真值=0,测纯布线偏移最直接) / 精密低阻标准 / 电化学工作站(EIS级仪器)测同一件做逐频真值。
