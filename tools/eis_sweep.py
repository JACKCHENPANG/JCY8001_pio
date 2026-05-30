#!/usr/bin/env python3
"""JCY8001 EIS 频率扫描 — 通过 Modbus RTU 驱动 dev 固件做阻抗谱。

用法:  python3 eis_sweep.py [port] [out.csv]
  默认 port=/dev/ttyUSB0, out=eis.csv

输出 CSV: Hz, RE_uOhm, IM_uOhm, wait_s   (IM 已是 -Z_imag, 直接用于 Nyquist)
画图/求 Rs: python3 eis_nyquist.py eis.csv

要点(实测标定):
- 频率写寄存器 0x4200 = Mantissa | Exp<<8;  Freq(Hz)=0.0074506*Mant*2^Exp。
  扫频点必须是固件频率表的有效 M/E(否则换算返0)。下面 PTS 已是 1kHz→0.119Hz 的 20 个有效点。
- 触发后【必须先等 zm_done(0x3E2D) 归 0】(dev 处理触发、复位), 再等回 1(完成)。
  否则会读到上一点的残值(寄存器还没刷新)——这是最易踩的坑。
- 转换时间是芯片硬底: ConvTime=max(1100, 1050*2^(7-exp)) ms。高频~2.5s, 0.1Hz~21s。
  fast 模式(0x4340=1)余量紧但仍新鲜(>=5周期); 标准(0)余量更足。
- 全 20 点 fast 模式实测 ~1.7 分钟。
"""
import serial, struct, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
OUT  = sys.argv[2] if len(sys.argv) > 2 else 'eis.csv'
SAMP_RES = 2      # 0x40C0 采样电阻档: 0=10Ω 1=5Ω 2=1Ω
FAST = 1          # 0x4340 速度: 1=快速 0=标准

# (freqval=Exp<<8|Mant, Hz) — 固件频率表有效点, 1kHz→0.119Hz
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
def tx(fc, reg, nv, exp=0):
    """写 Modbus 帧, 收到有效响应立刻返回(不固定等待)。exp=期望最小响应字节数。"""
    f = struct.pack('>BBHH', 1, fc, reg, nv); f += struct.pack('<H', crc(f))
    for _ in range(6):
        s.reset_input_buffer(); s.write(f)
        dl = time.time() + 0.35; buf = b''
        while time.time() < dl:
            buf += s.read(64)
            if len(buf) >= 5 and buf[0] == 1 and not (buf[1] & 0x80) and (exp == 0 or len(buf) >= exp):
                return buf
            time.sleep(0.005)
    return buf
def rd64(reg):
    r = tx(4, reg, 4, exp=13); return int.from_bytes(r[3:11], 'big', signed=True) if len(r) >= 11 else None
def rd16(reg):
    r = (tx(3, reg, 1, exp=7) if reg >= 0x3E00 else tx(4, reg, 1, exp=7))
    return int.from_bytes(r[3:5], 'big') if len(r) >= 5 else None

tx(6, 0x40C0, SAMP_RES); tx(6, 0x4340, FAST); tx(6, 0x4360, 0)   # 采样电阻 / 速度 / 转换门覆盖(0=自动)
T0 = time.time()
out = open(OUT, 'w'); out.write("Hz,RE_uOhm,IM_uOhm,wait_s\n")
for fv, hz in PTS:
    tx(6, 0x4200, fv)        # 设频率
    tx(5, 0x0000, 0xFF00)    # 触发 ZM
    t0 = time.time()
    while time.time() - t0 < 3:                  # 1) 等 zm_done 归 0 (dev 接受触发, 开始测量)
        if rd16(0x3E2D) == 0: break
        time.sleep(0.03)
    done = 0
    while time.time() - t0 < 150:                # 2) 等 zm_done=1 (测量完成)
        if rd16(0x3E2D) == 1: done = 1; break
        time.sleep(0.08)
    w = time.time() - t0; re = rd64(0x3000); im = rd64(0x3080)
    rev = re / 100000.0 if re is not None else float('nan')
    imv = im / 100000.0 if im is not None else float('nan')
    print("%.3fHz: RE=%.2f IM=%.2f uOhm (%.1fs, done=%d)" % (hz, rev, imv, w, done)); sys.stdout.flush()
    out.write("%.3f,%.2f,%.2f,%.1f\n" % (hz, rev, imv, w)); out.flush()
    tx(5, 0x0000, 0)         # 停 ZM (恢复 VM)
out.close()
print("=== 全 %d 点总耗时: %.1fs = %.2f 分钟  → %s ===" % (len(PTS), time.time()-T0, (time.time()-T0)/60, OUT))
