#!/usr/bin/env python3
"""JCY8001 EIS 扫频 — 固件自主扫频版 (一次性下发频点表, 固件逐点跑, 上位机边跑边取)。

对比 eis_sweep.py(上位机逐点下发逐点取回): 本脚本只下发一次频点表+触发,
之后轮询"已完成点数"边跑边读每个点的 RE/IM —— Modbus 来回大幅减少。
需固件支持自主扫频 (feat/firmware-sweep: 0x4400 频点表 / 0x43C0 点数 /
线圈 0x00C0 触发 / 0x3E40 状态 / 0x3E41 完成数 / 0x3400+ RE / 0x3500+ IM / 0x3640+ 频率码)。

用法:  python3 eis_sweep_fw.py [port] [out.csv]
  默认 port=/dev/ttyUSB0, out=eis_fw.csv
输出 CSV: Hz, RE_uOhm, IM_uOhm   (IM 已是 -Z_imag, 直接用于 Nyquist)
"""
import serial, struct, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
OUT  = sys.argv[2] if len(sys.argv) > 2 else 'eis_fw.csv'
SAMP_RES = 2      # 0x40C0 采样电阻档: 0=10Ω 1=5Ω 2=1Ω
FAST = 1          # 0x4340 速度: 1=快速 0=标准
AVG  = 1          # 0x4040 每点平均次数

# (freqval=Exp<<8|Mant, Hz) — 固件频率表有效点, 1kHz→0.119Hz (与 eis_sweep.py 同)
PTS = [(0x0B42,1007.083),(0x09A2,617.983),(0x08C6,377.656),(0x087A,232.697),(0x0796,143.052),
       (0x082E,87.738),(0x073A,55.313),(0x0812,34.332),(0x0716,20.981),(0x070E,13.351),
       (0x0806,11.444),(0x070A,9.537),(0x0902,7.629),(0x0706,5.722),(0x0802,3.815),
       (0x0702,1.907),(0x0602,0.954),(0x0502,0.477),(0x0402,0.238),(0x0302,0.119)]

s = serial.Serial(PORT, 115200, timeout=0.03)
def crc(d):
    c = 0xFFFF
    for b in d:
        c ^= b
        for _ in range(8): c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c
def _send(f, exp):
    f += struct.pack('<H', crc(f))
    for _ in range(6):
        s.reset_input_buffer(); s.write(f)
        dl = time.time() + 0.35; buf = b''
        while time.time() < dl:
            buf += s.read(64)
            if len(buf) >= 5 and buf[0] == 1 and not (buf[1] & 0x80) and (exp == 0 or len(buf) >= exp):
                return buf
            time.sleep(0.005)
    return buf
def tx(fc, reg, nv, exp=0):
    return _send(struct.pack('>BBHH', 1, fc, reg, nv), exp)
def tx_multi(reg, values, exp=8):
    """FC10 写多个寄存器: reg 起始, values 列表。"""
    body = struct.pack('>BBHHB', 1, 0x10, reg, len(values), len(values) * 2)
    for v in values: body += struct.pack('>H', v & 0xFFFF)
    return _send(body, exp)
def rd64(reg):
    r = tx(4, reg, 4, exp=13); return int.from_bytes(r[3:11], 'big', signed=True) if len(r) >= 11 else None
def rd16(reg):
    r = (tx(3, reg, 1, exp=7) if reg >= 0x3E00 else tx(4, reg, 1, exp=7))
    return int.from_bytes(r[3:5], 'big') if len(r) >= 5 else None

N = len(PTS)
tx(6, 0x40C0, SAMP_RES); tx(6, 0x4340, FAST); tx(6, 0x4040, AVG); tx(6, 0x4360, 0)
tx_multi(0x4400, [fv for fv, _ in PTS])   # 1) 一次性下发频点表 (N 个 M/E 码)
tx(6, 0x43C0, N)                          # 2) 频点数
tx(5, 0x00C0, 0xFF00)                     # 3) 触发整段扫频
print("下发 %d 点频点表, 固件自主扫频中..." % N); sys.stdout.flush()

T0 = time.time()
out = open(OUT, 'w'); out.write("Hz,RE_uOhm,IM_uOhm\n")
got = 0
while time.time() - T0 < 600:             # 4) 轮询已完成点数, 边跑边读
    done_cnt = rd16(0x3E41) or 0
    while got < done_cnt and got < N:      # 有新完成点 → 立即取回
        re = rd64(0x3400 + got * 4); im = rd64(0x3500 + got * 4)
        rev = re / 100000.0 if re is not None else float('nan')
        imv = im / 100000.0 if im is not None else float('nan')
        hz = PTS[got][1]
        print("  [%2d/%d] %.3fHz: RE=%.2f IM=%.2f uOhm (%.1fs)" %
              (got + 1, N, hz, rev, imv, time.time() - T0)); sys.stdout.flush()
        out.write("%.3f,%.2f,%.2f\n" % (hz, rev, imv)); out.flush()
        got += 1
    if rd16(0x3E40) == 2 and got >= N: break   # 状态=完成 且全部取回
    time.sleep(0.1)
tx(5, 0x00C0, 0)                          # 5) 停
out.close()
print("=== %d 点固件扫频总耗时: %.1fs = %.2f 分钟  → %s ===" %
      (got, time.time() - T0, (time.time() - T0) / 60, OUT))
