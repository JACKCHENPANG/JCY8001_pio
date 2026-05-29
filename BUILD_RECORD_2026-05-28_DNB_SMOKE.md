# JCY8001 DNB1101 Smoke Record - 2026-05-28

## Firmware

- Source path: `/Users/jcy/Projects/hardware/JCY8001/JCY8001_pio`
- Git HEAD before local edits: `bc70f17 v1.3: DNB1101 GetData protocol...`
- Local source: modified `src/main.c`
- Firmware bin SHA256: `ead258321cf23ef1db801c139c7505393dc175883422619d98bb7002a763aa1d`
- Build: PlatformIO `genericSTM32F103RC`, success
- Size: Flash 2392 bytes, RAM 328 bytes

## Safety audit

- Source SWD/RDP keyword audit: OK, no `AFIO_MAPR`, `0x40010004`, `SWJ_CFG`, `0x04000000`, `GPIO_Remap_SWJ_Disable`, `FLASH_OB`, `0x1FFFF800` in `src/include/lib/platformio.ini`.
- J-Link post-flash reconnect: OK
- Option bytes after flash: `1FFFF800 = FFFF5AA5 FFFFFFFF FFFFFFFF FFFFFFFF`
- Fault status after flash: `CFSR=0`, `HFSR=0`
- Config marker written and verified: `0x08004000 = 0x12345678`

## Flash command summary

Remote host: `ubuntu@192.168.0.53`

```text
erase
loadfile /tmp/firmware.bin 0x08000000
w4 0x08004000 0x12345678
r
g
```

## Register verification

Post-flash J-Link:

```text
E0042000 = 10036414
1FFFF800 = FFFF5AA5 FFFFFFFF FFFFFFFF FFFFFFFF
08004000 = 12345678
E000ED28 = 00000000
E000ED2C = 00000000
40013000 = 00000354 00000000 00000002 000000FF
```

SPI1 CR1 is back to the proven v1.1/v1.2 value `0x0354`. Trial value `0x036C` did not improve DNB response and still read MISO-like `0xFF` data.

## Modbus smoke result

Serial: `/dev/ttyUSB0`, 115200 8N1, addr=1.

Stable Modbus communication:

```text
0x3E00 ch_count = 1
0x3E02 fw_ver   = 0x0204
0x3380 status   = 0x0003
0x3300 temp     = 0
0x3340 voltage  = 0
0x3200 VZM      = 0
```

DNB debug registers `0x3E20..0x3E26` repeatedly:

```text
[0x00FF, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xF0FF]
```

Interpretation:

- USART2/CP2102/Modbus path is working.
- Firmware is running and not faulting.
- DNB1101 enumeration/GetStatus is not returning a valid frame.
- Raw response remains `0xFFFFFFFF`/tail pattern, so the MCU is effectively reading idle-high/no valid DNB response on MISO.
- Therefore 6V voltage and temperature were not measured yet: both remain zero through Modbus.

## Conclusion

Current blocker is below Modbus layer: DNB1101 SPI physical link, DNB power/reference, chip select/wake/timing, or frame format/offset. Next useful step is waveform capture of SPI SCK/MOSI/MISO/CS during enumeration. ISDS210A was not reachable on checked hosts in this run; 192.168.0.53 only exposed `/dev/ttyUSB0` and no ISDS/Hantek USB device.
