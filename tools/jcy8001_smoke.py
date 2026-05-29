#!/usr/bin/env python3
"""JCY8001 Modbus RTU smoke test.

Default target: Linux test host 192.168.0.53 /dev/ttyUSB0, 115200 8N1.
Run on the host connected to CP2102, or copy this script there.
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
from dataclasses import dataclass

try:
    import serial  # type: ignore
except Exception as exc:  # pragma: no cover
    print(f"pyserial import failed: {exc}", file=sys.stderr)
    print("Install with: python3 -m pip install pyserial", file=sys.stderr)
    raise


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


def frame(addr: int, fc: int, payload: bytes) -> bytes:
    body = bytes([addr & 0xFF, fc & 0xFF]) + payload
    return body + struct.pack("<H", crc16(body))


@dataclass
class ModbusRTU:
    port: str
    baud: int = 115200
    addr: int = 1
    timeout: float = 0.5
    gap: float = 0.08

    def __post_init__(self) -> None:
        self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout)
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        time.sleep(0.05)

    def close(self) -> None:
        self.ser.close()

    def txrx(self, fc: int, payload: bytes, expect_min: int = 5) -> bytes:
        req = frame(self.addr, fc, payload)
        self.ser.reset_input_buffer()
        self.ser.write(req)
        self.ser.flush()
        time.sleep(self.gap)
        resp = self.ser.read(max(expect_min, 256))
        if not resp:
            raise TimeoutError(f"no response for FC{fc:02X} TX={req.hex(' ')}")
        if len(resp) < 5:
            raise IOError(f"short response FC{fc:02X}: {resp.hex(' ')}")
        got = int.from_bytes(resp[-2:], "little")
        calc = crc16(resp[:-2])
        if got != calc:
            raise IOError(f"crc error FC{fc:02X}: rx={resp.hex(' ')} got={got:04x} calc={calc:04x}")
        if resp[0] != self.addr:
            raise IOError(f"address mismatch: rx={resp.hex(' ')}")
        if resp[1] & 0x80:
            raise IOError(f"modbus exception FC{fc:02X}: code={resp[2]:02X} rx={resp.hex(' ')}")
        if resp[1] != fc:
            raise IOError(f"function mismatch: rx={resp.hex(' ')}")
        return resp

    def read_bits(self, fc: int, start: int, count: int) -> list[int]:
        resp = self.txrx(fc, struct.pack(">HH", start, count), expect_min=5 + ((count + 7) // 8))
        n = resp[2]
        data = resp[3:3 + n]
        return [(data[i // 8] >> (i % 8)) & 1 for i in range(count)]

    def read_regs(self, fc: int, start: int, count: int) -> list[int]:
        resp = self.txrx(fc, struct.pack(">HH", start, count), expect_min=5 + count * 2)
        n = resp[2]
        if n != count * 2:
            raise IOError(f"byte count mismatch: expected {count*2}, got {n}, rx={resp.hex(' ')}")
        return [int.from_bytes(resp[3 + i * 2:5 + i * 2], "big") for i in range(count)]

    def write_coil(self, addr: int, on: bool) -> bytes:
        val = 0xFF00 if on else 0x0000
        return self.txrx(0x05, struct.pack(">HH", addr, val), expect_min=8)

    def write_reg(self, addr: int, value: int) -> bytes:
        return self.txrx(0x06, struct.pack(">HH", addr, value & 0xFFFF), expect_min=8)


def show_regs(label: str, regs: list[int], scale: float | None = None) -> None:
    hexs = " ".join(f"0x{x:04X}" for x in regs)
    if len(regs) == 1 and scale:
        print(f"{label:<16} {hexs:<12} => {regs[0] / scale:g}")
    else:
        print(f"{label:<16} {hexs}")


def run_smoke(args: argparse.Namespace) -> int:
    m = ModbusRTU(args.port, args.baud, args.addr, args.timeout, args.gap)
    try:
        print(f"JCY8001 smoke: port={args.port} baud={args.baud} addr={args.addr}")
        show_regs("ch_count 3E00", m.read_regs(0x04, 0x3E00, 1))
        show_regs("version 3E01", m.read_regs(0x04, 0x3E01, 1))
        show_regs("fw_ver 3E02", m.read_regs(0x04, 0x3E02, 1))
        show_regs("build 3E04", m.read_regs(0x04, 0x3E04, 1))
        show_regs("freq 4000", m.read_regs(0x03, 0x4000, 1))
        show_regs("avg 4040", m.read_regs(0x03, 0x4040, 1))
        show_regs("coil 0000", m.read_bits(0x01, 0x0000, 1))
        show_regs("di 1000", m.read_bits(0x02, 0x1000, 1))

        if args.start:
            print("write coil 0000 = ON")
            print("  rx", m.write_coil(0x0000, True).hex(" "))
            time.sleep(args.wait)
        elif args.stop:
            print("write coil 0000 = OFF")
            print("  rx", m.write_coil(0x0000, False).hex(" "))
            time.sleep(0.1)

        show_regs("temp 3300", m.read_regs(0x04, 0x3300, 1), scale=10)
        show_regs("volt 3340", m.read_regs(0x04, 0x3340, 1), scale=10000)
        show_regs("status 3380", m.read_regs(0x04, 0x3380, 1))
        show_regs("dnb_dbg 3E20", m.read_regs(0x04, 0x3E20, 7))
        show_regs("re 3000", m.read_regs(0x04, 0x3000, 4))
        show_regs("im 3080", m.read_regs(0x04, 0x3080, 4))
        show_regs("vzm 3200", m.read_regs(0x04, 0x3200, 2))
        print("OK")
        return 0
    finally:
        m.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--addr", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=0.5)
    ap.add_argument("--gap", type=float, default=0.08)
    ap.add_argument("--start", action="store_true", help="write FC05 coil 0000=ON then read values")
    ap.add_argument("--stop", action="store_true", help="write FC05 coil 0000=OFF then read values")
    ap.add_argument("--wait", type=float, default=1.5)
    return run_smoke(ap.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
