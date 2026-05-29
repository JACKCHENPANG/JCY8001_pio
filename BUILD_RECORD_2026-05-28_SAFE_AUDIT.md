# JCY8001 firmware safe audit record

Time: 2026-05-28T15:32:09

## Firmware

- Path: `/Users/jcy/Projects/hardware/JCY8001/JCY8001_pio/.pio/build/genericSTM32F103RC/firmware.elf`
- Size: `137716` bytes
- SHA256: `b3fd2cb0f71a65e1d68907d413c29e7843f1e4c63de1c0790975dfae47864995`
- Git HEAD: `bc70f17`
- Git status:

```text
## main...origin/main [ahead 4]
```

## Build result

`pio run` succeeded.

- RAM used: 320 bytes / 49152 bytes
- Flash used: 1756 bytes / 262144 bytes

## SWD/RDP safety audit

- Source tree scan: no `AFIO_MAPR`, `0x40010004`, `SWJ_CFG`, `FLASH_OB`, or `RDP` usage in `JCY8001_pio` source.
- ELF byte scan: little-endian `0x40010004` not found.
- ELF byte scan: option-byte base `0x1FFFF800` not found.
- Disassembly scan: no references to `40010004`, `1ffff800`, `4002201c`, `40022020`, `4002202c`.

Conclusion: this PlatformIO firmware does not contain the known dangerous SWD-disable write. It is still an experimental DNB1101/Modbus firmware and must be flashed only with pre/post J-Link verification.

## Flashing rule

Before flash:

1. J-Link connect under SWD and halt.
2. Read `0x1FFFF800` Option Bytes; RDP must stay `0xA5`.
3. Record firmware SHA256 and Git status.

After flash:

1. Reconnect J-Link/SWD immediately.
2. Read `DBGMCU_IDCODE` and Option Bytes again.
3. Verify USART2/CP2102 Modbus on `/dev/ttyUSB0`.
