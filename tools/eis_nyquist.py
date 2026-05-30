#!/usr/bin/env python3
"""读 eis_sweep.py 的 CSV, 画 Nyquist 图, 求 Rs(虚部过零点 = 欧姆/串联内阻)。

用法: python3 eis_nyquist.py eis.csv [out.png]

约定: CSV 的 IM 已是 -Z_imag(固件 zm_im = -zm_real), 直接作 Nyquist 纵轴, 不要再取反。
Rs = Z_real where Im=0 (高频段虚部由负穿零到正之处), 线性插值。
"""
import sys, csv
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt

CSV = sys.argv[1] if len(sys.argv) > 1 else 'eis.csv'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'nyquist.png'

hz, re, im = [], [], []
for row in csv.DictReader(open(CSV)):
    try:
        h = float(row['Hz']); r = float(row['RE_uOhm']); i = float(row['IM_uOhm'])
    except (ValueError, KeyError):
        continue
    if r != r or i != i:  # NaN 跳过
        continue
    hz.append(h); re.append(r); im.append(i)

# Rs: 找虚部由 <0 穿到 >=0 的相邻两点, 线性插值
Rs = fz = None
for k in range(len(im) - 1):
    if im[k] < 0 <= im[k + 1]:
        f = (-im[k]) / (im[k + 1] - im[k])
        Rs = re[k] + f * (re[k + 1] - re[k]); fz = hz[k] + f * (hz[k + 1] - hz[k]); break
if Rs is None:
    Rs = min(re); fz = float('nan'); print("警告: 没找到虚部过零点, Rs 取最小 Z_real")
print("Rs = %.1f uOhm = %.4f mOhm  @ %.2f Hz" % (Rs, Rs / 1000, fz))

fig, ax = plt.subplots(figsize=(8.5, 6.2))
ax.plot(re, im, 'o-', color='#1f77b4', ms=5, lw=1.3, label='measured (%d pts)' % len(re))
ax.plot([Rs], [0], 'r*', ms=20, label='Rs (Im=0) = %.0f uOhm = %.3f mOhm' % (Rs, Rs / 1000))
ax.axhline(0, color='gray', lw=0.6, ls='--')
ax.set_xlabel('Z_real  (uOhm)'); ax.set_ylabel('-Z_imag  (uOhm)')
ax.set_title('JCY8001 EIS Nyquist   Rs = %.0f uOhm = %.3f mOhm @ %.1f Hz' % (Rs, Rs / 1000, fz))
ax.grid(True, alpha=0.3); ax.legend(loc='best'); ax.set_aspect('equal', adjustable='datalim')
plt.tight_layout(); plt.savefig(OUT, dpi=130)
print("saved", OUT)
