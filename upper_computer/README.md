# JCY8001 单通道 EIS 上位机 (PyQt5)

JCY8001 单通道电化学阻抗谱分析仪图形上位机。Modbus RTU over 串口，支持固件自主扫频
(频点表一次下发→固件逐点测→边跑边读) + 单点测量 + 实时 Nyquist/Bode + CSV 导出 + 采样电阻校准。

## 运行
```
pip install -r requirements.txt
python3 main.py
```

## 协议
见 `../docs/MODBUS_PROTOCOL.md`。固件自主扫频寄存器：频点表 0x4400 / 点数 0x43C0 /
触发线圈 0x00C0 / 状态 0x3E40 / 已完成点数 0x3E41 / 结果 RE 0x3400+idx*4·IM 0x3500+idx*4。

## 打包 exe
```
pyinstaller --onefile --windowed main.py
```
