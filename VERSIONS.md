# JCY8001 固件版本记录

| 版本 | 日期 | Flash | RAM | 功能 | 验证 |
|------|------|-------|-----|------|------|
| v1.0 | 0504 | 1256B | 308B | Modbus RTU (FC01/03/04/06), HSI 8MHz | ✅ ALL OK |
| v1.1 | 0504 | 1284B | 308B | +SPI1 驱动(SSM模式), CR1=0x0354 | ✅ ALL OK |
| v1.2 | 0504 | 1572B | 308B | +DNB1101枚举, 检测到1个IC, 通道数→0x3E00 | ✅ ALL OK |
| v1.3 | 0504 | 1964B | 320B | +DNB1101 Init + GetData(MainVolt/MainDieTemp/VZM) 500ms循环 → Modbus寄存器 | ⏳ 待烧录验证 |
| v1.4 | - | - | - | EEPROM 参数存储 | ⏳ |
| v1.5 | - | - | - | 完整寄存器对齐 combined.hex | ⏳ |
