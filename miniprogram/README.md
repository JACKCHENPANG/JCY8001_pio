# JCY8001 电化学阻抗谱 微信小程序

Modbus over BLE（JDY-10 透传模块）。手机连 JCY8001，读固件算好的 RE/IM，本地做
L 补偿 / Rs-Rct 半圆拟合 / 等效电路(ECM) / 整包一致性判定。

## 硬件
- BLE 模块 **JDY-10**（Service `FFE0` / Char `FFE1`，UART 默认 9600）插板上 **J12**（USART1，PA9/PA10）。
- 固件 ≥ **v2.20**：USART1 已开 Modbus、波特率 9600 匹配 JDY-10，即插即用。
- 接线：J12 VCC/GND/TX↔模块RX/RX↔模块TX。

## 打开 / 运行
1. 微信开发者工具导入本目录（`miniprogram/`）。
2. `project.config.json` 把 `appid` 改成你公司的小程序 AppID（开发可用测试号）。
3. 真机调试（蓝牙需真机，模拟器连不了 BLE）。

## 结构
- `utils/modbus.js` — Modbus RTU 编解码（CRC16 / FC03/05/06/10 / 64位μΩ）。寄存器图对齐固件 `src/main.c`。
- `utils/analysis.js` — L 补偿 / 剔高频伪迹 / Rct 半圆拟合 / ECM(LM) / 一致性。**已 node 验证与 PC 工具逐位一致**（L=218nH/Rs=852/Rct=385；ECM Rs=846/Rct=342/Cdl=9.5F/n=0.83/RMS1.23%）。
- `utils/ble.js` — JDY-10 BLE 连接 + Modbus 请求-响应（BLE 分包重组）。
- `utils/device.js` — 高层：读基础信息、固件自主扫频（对齐 `tools/eis_sweep_fw.py`）。
- `pages/index` — 连接 + 单点全频谱 + Nyquist + Rs/Rct/ECM。
- `pages/consistency` — 不拆包逐串一致性向导（排针引导 + RMS 门 + 整包对比 + 复制报告）。

## 备注
- BLE 单包 ≤20 字节，已自动分片；Modbus 帧小，无吞吐压力。
- 若模块不是 JDY-10：多数透传模块同为 `FFE0/FFE1`，改 `ble.js` 的 `SVC/CHR` 即可；UART 波特率与固件 `USART1->BRR` 对齐。
