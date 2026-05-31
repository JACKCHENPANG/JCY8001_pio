# JCY8001 OTA Bootloader

分区: Bootloader @0x08000000 (16KB) + App @0x08004000 (238KB) + 标定数据 @0x0803F800.

## 编译 Bootloader
```
arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -Os -ffreestanding -nostdlib -nostartfiles \
  -Wl,-Tboot.ld boot.c -o boot.elf
arm-none-eabi-objcopy -O binary boot.elf boot.bin
```
App 由 PlatformIO 用 ldscript_app_0x4000.ld 编到 0x08004000 (platformio.ini board_build.ldscript), 并在 main() 设 SCB->VTOR=0x08004000.

## 首次烧录 (J-Link, 一次性)
```
loadfile boot.bin 0x08000000
loadfile firmware.bin 0x08004000   # 注意: 不要再写旧的 0x08004000 app标志!
```
之后所有升级走 USB OTA, 不用 J-Link.

## Bootloader 升级协议 (Modbus RTU addr=1)
| 寄存器/线圈 | 含义 |
|---|---|
| App线圈 0x0020 (FC05=1) | App命令: 置BKP标志+复位进Bootloader |
| 0x7000 (FC03) | 读=0xB007 表示在Bootloader升级模式 |
| 0x7001 (FC03) | 状态 0=idle 1=已擦除 2=写入中 3=校验OK 0xE=CRC错 |
| 0x7002/0x7003 (FC03) | 已写入偏移(hi/lo) |
| 0x7010 (FC16 4寄存器) | [len_hi,len_lo,crc32_hi,crc32_lo] 设长度+CRC32 |
| 0x7020 (FC06) | 1=擦除App区(按长度算页) 2=完成(CRC32校验→跳App) |
| 0x7100 (FC16 N寄存器) | 固件数据块, 每寄存器=小端半字(=bin[2i]|bin[2i+1]<<8), 顺序写, 偏移自增 |

防变砖: CRC32 不过不跳App; App无效(SP/PC非法)Bootloader不跳; J-Link随时可救.

上位机工具: tools/ota_flash.py
