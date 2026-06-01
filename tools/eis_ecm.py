#!/usr/bin/env python3
"""EIS 等效电路(ECM)自动拟合 — Randles + CPE + Warburg。

模型:  Z(ω) = jωL + Rs + [ CPE  ‖  (Rct + Zw) ]
  L    : 夹具/引线串联电感
  Rs   : 欧姆内阻 (高频实轴截距)
  Rct  : 电荷转移电阻 (半圆直径)
  CPE  : 双电层 (常相位元件) Z=1/(Q(jω)^n); n=1 即理想电容, n<1 = 压扁弧
  Zw   : Warburg 扩散 Z=σ(jω)^-0.5 (低频 45° 尾)
派生: Cdl_eff (Brug), 弧峰特征频率 f_peak。

用法: python3 eis_ecm.py eis.csv [out.png]
约定: CSV 的 IM 已是 -Z''(Z=RE + j·(-IM))。自动剔除高频互感伪迹(Z'min 以上)后拟合。
依赖: numpy scipy (画图需 matplotlib)。
"""
import sys, csv, math
import numpy as np
from scipy.optimize import least_squares

CSV = sys.argv[1] if len(sys.argv) > 1 else 'eis.csv'
OUT = sys.argv[2] if len(sys.argv) > 2 else None

hz, re, im = [], [], []
for row in csv.DictReader(open(CSV)):
    try:
        h = float(row['Hz']); r = float(row['RE_uOhm']); i = float(row['IM_uOhm'])
    except (ValueError, KeyError):
        continue
    if r != r or i != i or r <= 0:
        continue
    hz.append(h); re.append(r); im.append(i)
hz = np.array(hz); re = np.array(re); im = np.array(im)
w = 2 * np.pi * hz
Z = (re - 1j * im) * 1e-6                      # Ω

i_min = int(np.argmin(re))                     # 剔高频互感伪迹 (Z'min 以上)
keep = np.arange(i_min, len(hz))
wf, Zf = w[keep], Z[keep]
if i_min:
    print('剔除高频伪迹 %d 点: %s' % (i_min, ', '.join('%.0fHz' % hz[k] for k in range(i_min))))


def model(p, w):
    L, Rs, Rct, Q, n, sig = p
    jw = 1j * w
    Zcpe = 1.0 / (Q * (jw) ** n)
    Zw = sig * (jw) ** -0.5
    Zp = 1.0 / (1.0 / Zcpe + 1.0 / (Rct + Zw))
    return 1j * w * L + Rs + Zp


def resid(p):
    r = (model(p, wf) - Zf) / np.abs(Zf)       # 相对残差
    return np.concatenate([r.real, r.imag])


# 初值: L/Rs/Rct 粗估 + CPE/Warburg 经验值
Rs0 = re[i_min] * 1e-6
Rct0 = max((re[-1] - re[i_min]) * 1e-6 * 0.8, 1e-5)
p0 = [2e-7, Rs0, Rct0, 10.0, 0.85, 5e-5]
lb = [1e-9, 1e-6, 1e-6, 1e-3, 0.3, 0.0]
ub = [1e-6, 5e-3, 5e-3, 1e5, 1.0, 1e-1]
res = least_squares(resid, p0, bounds=(lb, ub), max_nfev=30000)
L, Rs, Rct, Q, n, sig = res.x
Cdl = (Q * (1 / Rs + 1 / Rct) ** (n - 1)) ** (1 / n)     # Brug 等效 Cdl
fpk = 1 / (2 * np.pi * (Rct * Cdl))
rms = np.sqrt(np.mean(np.abs((model(res.x, wf) - Zf) / np.abs(Zf)) ** 2)) * 100

print('── ECM 拟合 (RMS 相对误差 %.2f%%, %d 点) ──' % (rms, len(keep)))
print('L   = %.0f nH    (夹具串感)' % (L * 1e9))
print('Rs  = %.1f uOhm = %.4f mOhm  (欧姆内阻)' % (Rs * 1e6, Rs * 1e3))
print('Rct = %.1f uOhm = %.4f mOhm  (电荷转移)' % (Rct * 1e6, Rct * 1e3))
print('CPE : Q=%.3g  n=%.3f  -> Cdl_eff=%.3g F  (f_peak~%.1f Hz)' % (Q, n, Cdl, fpk))
print('Warburg sigma = %.4g Ohm/sqrt(s)  (低频扩散)' % sig)

if OUT:
    import matplotlib; matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    wm = np.logspace(np.log10(wf.min()), np.log10(wf.max()), 400)   # 仅可信频段, 避免HF电感尾占满视野
    Zm = model(res.x, wm)
    fig, ax = plt.subplots(figsize=(8.6, 6.4))
    ax.plot(Zf.real * 1e6, -Zf.imag * 1e6, 'o', color='#1f6feb', ms=6, label='measured (trusted)')
    ax.plot(Zm.real * 1e6, -Zm.imag * 1e6, '-', color='#cf222e', lw=1.6, label='ECM fit')
    ax.plot([Rs * 1e6], [0], 'r*', ms=16)
    ax.axhline(0, color='#bbb', lw=0.6); ax.grid(True, alpha=0.3)
    ax.set_xlabel("Z' (uOhm)"); ax.set_ylabel("-Z'' (uOhm)"); ax.legend(loc='upper left', fontsize=9)
    # 锁定到弧区视野
    yv = np.r_[-Zf.imag * 1e6, -Zm.imag * 1e6]
    ax.set_ylim(min(yv.min(), -20) - 20, yv.max() + 40)
    ax.set_xlim(Rs * 1e6 - 30, re[keep].max() + 40)
    box = ('ECM: L-Rs-(Rct||CPE)-W\n'
           'L=%.0f nH\nRs=%.0f uOhm\nRct=%.0f uOhm\n'
           'Q=%.3g  n=%.2f\nCdl=%.2g F\nsigma=%.3g\nRMS=%.2f%%'
           % (L * 1e9, Rs * 1e6, Rct * 1e6, Q, n, Cdl, sig, rms))
    ax.text(0.97, 0.03, box, transform=ax.transAxes, fontsize=8.5, va='bottom', ha='right',
            family='monospace', bbox=dict(boxstyle='round', fc='#fffbe6', ec='#d29922'))
    ax.set_title('JCY8001 EIS equivalent-circuit fit')
    plt.tight_layout(); plt.savefig(OUT, dpi=130)
    print('saved', OUT)
