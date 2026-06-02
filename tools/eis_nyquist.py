#!/usr/bin/env python3
"""读 eis_sweep*.py 的 CSV, 画 Nyquist + 自动拟合并补偿夹具串联电感 L, 求 Rs/Rct。

用法:
  python3 eis_nyquist.py eis.csv [out.png] [--hfmin 300] [--lcal lcal.txt] [--no-comp]

约定: CSV 的 IM 已是 -Z_imag(固件 zm_im = -zm_real), 直接作 Nyquist 纵轴, 不再取反。

电感补偿:
  测到的阻抗 Z = Z_cell + jwL (夹具/引线串联电感). 高频段电芯近纯阻,
  虚部 Z'' ≈ wL → L = Z''/w. 取高频(>=hfmin Hz)点最小二乘拟合 Z''=wL,
  再从整条谱减去 jwL (实部不动): IM_comp = IM + w*L.
  --lcal 指定外部短路标定得到的 L(nH), 优先于自动拟合.
  --no-comp 关闭补偿, 只画原始.

一致性自检: 各高频点单独算 L_i=Z''_i/w_i, 看离散度 CV=std/mean.
  CV<5% 优(可信) / 5~15% 一般 / >15% 警告(夹具噪声或接触松, L 不可信).
"""
import sys, csv, math

import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = sys.argv[1:]
CSV = args[0] if len(args) > 0 else 'eis.csv'
OUT = args[1] if len(args) > 1 else 'nyquist.png'
def opt(name, default):
    if name in opts:
        i = opts.index(name)
        return opts[i + 1] if i + 1 < len(opts) else default
    return default
HF_MIN = float(opt('--hfmin', '300'))
LCAL = opt('--lcal', None)
DO_COMP = '--no-comp' not in opts

hz, re, im = [], [], []
for row in csv.DictReader(open(CSV)):
    try:
        h = float(row['Hz']); r = float(row['RE_uOhm']); i = float(row['IM_uOhm'])
    except (ValueError, KeyError):
        continue
    if r != r or i != i or r <= 0:   # 跳过 NaN / 丢点(RE=0)
        continue
    hz.append(h); re.append(r); im.append(i)

# ── 拟合 / 载入 夹具串联电感 L ─────────────────────────────────────────────
L_nH = None; cv = None; verdict = ''
if LCAL:
    try:
        L_nH = float(open(LCAL).read().split()[0]); verdict = '外部短路标定'
    except Exception as e:
        print('警告: 读 lcal 失败(%s), 改用自动拟合' % e); LCAL = None
if L_nH is None and DO_COMP:
    hf = [(h, -i) for h, i in zip(hz, im) if h >= HF_MIN]   # (f, Z''=-IM)
    if len(hf) >= 2:
        # 最小二乘过原点拟合 Z''[Ω] = w*L  →  L = sum(Z''*w)/sum(w^2). Z'' 由 uΩ 转 Ω(*1e-6)
        num = sum((zpp * 1e-6) * (2 * math.pi * f) for f, zpp in hf)
        den = sum((2 * math.pi * f) ** 2 for f, _ in hf)
        L = num / den                                        # H
        if L <= 0:
            # 高频点已是容性 (高内阻电芯, 电感相对可忽略) → 无感性尾, 不补偿
            L_nH = 0.0; cv = None; verdict_en = 'negligible'
            print('高频无感性尾 (拟合 L≤0): 高内阻电芯, 电感可忽略, L=0 不补偿')
        else:
            Li = [(zpp * 1e-6) / (2 * math.pi * f) for f, zpp in hf]   # 各点单独的 L (H)
            mean = sum(Li) / len(Li)
            sd = (sum((x - mean) ** 2 for x in Li) / len(Li)) ** 0.5
            cv = sd / mean if mean else 0
            L_nH = L * 1e9
            verdict = ('优·可信' if cv < 0.05 else '一般' if cv < 0.15 else '⚠警告·L不可信')
            verdict_en = ('reliable' if cv < 0.05 else 'fair' if cv < 0.15 else 'WARN: L unreliable')
            print('L 拟合: %.1f nH  (高频%d点, CV=%.1f%% → %s)' % (L_nH, len(hf), cv * 100, verdict))
            for f, zpp in hf:
                print('   %.0fHz  L=%.1f nH' % (f, (zpp * 1e-6) / (2 * math.pi * f) * 1e9))
            if cv >= 0.15:
                print('⚠ 高频点 L 离散 >15%: 夹具噪声/接触松, 该缩短引线或做短路标定, 本次补偿仅供参考')
    else:
        print('警告: 高频(>=%.0fHz)点不足2个, 无法拟合 L, 跳过补偿' % HF_MIN); DO_COMP = False

# ── 补偿 ──────────────────────────────────────────────────────────────────
im_c = im
if DO_COMP and L_nH is not None:
    L = L_nH * 1e-9
    im_c = [i + (2 * math.pi * h) * L * 1e6 for h, i in zip(hz, im)]   # IM_comp (uOhm)

def zero_cross_Rs(ylist):
    # Rs = 高频侧实轴截距. 从高频(index0)往低频扫, 取第一处虚部变号(任意方向)。
    for k in range(len(ylist) - 1):
        a, b = ylist[k], ylist[k + 1]
        if a == 0:
            return re[k], hz[k]
        if (a < 0) != (b < 0):           # 变号
            f = (-a) / (b - a)
            return re[k] + f * (re[k + 1] - re[k]), hz[k] + f * (hz[k + 1] - hz[k])
    return min(re), float('nan')

# ── 高频伪迹识别 + Rct 半圆拟合求 Rs/Rct ─────────────────────────────────────
# 互感/引线耦合使实部 Z' 在高频自己翘起(-jωL 只动虚部, 补不掉实部翘起)→ 标灰剔除。
# 剪掉伪迹后可信弧不过零(最低频点仍在 -Z''>0), 所以 Rs 不能直接读虚部过零,
# 改为对 Rct 半圆做最小二乘圆拟合, 取左实轴截距=Rs, 右截距-Rs=Rct(压扁半圆也适用)。
KEEP_HF = '--keep-hf' in opts
i_min = min(range(len(re)), key=lambda k: re[k])
Rct = None; circle = None; arc = []
if DO_COMP and L_nH is not None and not KEEP_HF:
    artifact = list(range(0, i_min))          # 列表按 HF->LF, 比 Z'min 更高频 = 伪迹
    if artifact:
        print('高频伪迹点(实部翘起, 标灰剔除): ' + ', '.join('%.0fHz' % hz[k] for k in artifact))
    tr = list(range(i_min, len(re)))          # 可信点
    cand = [k for k in tr if hz[k] >= 5] or tr
    peak = max(cand, key=lambda k: im_c[k])   # Rct 弧峰(中频局部极大, 避开 Warburg 尖)
    we = len(re) - 1
    for k in range(peak, len(re) - 1):
        if im_c[k + 1] > im_c[k]:              # 峰后第一个谷 = Warburg 起点
            we = k; break
    arc = list(range(i_min, we + 1))           # Rct 半圆 (不含 Warburg 上翘)
    Rs, fz, Rct = re[i_min], hz[i_min], None    # 默认回退 = Z'min
    if len(arc) >= 4:
        x = np.array([re[k] for k in arc]); y = np.array([im_c[k] for k in arc])
        A = np.c_[x, y, np.ones_like(x)]; b = -(x ** 2 + y ** 2)
        D, E, F = np.linalg.lstsq(A, b, rcond=None)[0]
        xc, yc = -D / 2, -E / 2; disc = D * D - 4 * F
        if disc > 0:
            r1 = (-D - disc ** 0.5) / 2; r2 = (-D + disc ** 0.5) / 2
            Rs, Rsum = min(r1, r2), max(r1, r2); Rct = Rsum - Rs; fz = float('nan')
            circle = (xc, yc, (xc ** 2 + yc ** 2 - F) ** 0.5)
            print('Rct 半圆拟合: 圆心(%.0f,%.0f) → Rs=%.1f uOhm  Rct=%.1f uOhm  (Rs+Rct=%.1f)'
                  % (xc, yc, Rs, Rct, Rsum))
    print('Rs = %.1f uOhm = %.4f mOhm%s' % (Rs, Rs / 1000,
          ('  Rct=%.0f uOhm (半圆外推到 Z\'\'=0)' % Rct) if Rct else '  (Z\'min 回退)'))
else:
    Rs, fz = zero_cross_Rs(im_c if DO_COMP else im)
    artifact = []
    print('Rs = %.1f uOhm = %.4f mOhm @ %.2f Hz' % (Rs, Rs / 1000, fz))

# ── 画图 ──────────────────────────────────────────────────────────────────
if DO_COMP and L_nH is not None:
    fig, (axr, axc) = plt.subplots(1, 2, figsize=(12, 5.2))
    axr.plot(re, im, 'o-', color='#8b949e', ms=4, lw=1.2)
    axr.axhline(0, color='#bbb', lw=0.6); axr.grid(True, alpha=0.3)
    axr.set_xlabel("Z' (uOhm)"); axr.set_ylabel("-Z'' (uOhm)")
    axr.set_title('RAW  (HF tail = L=%.0f nH fixture)' % L_nH)
    tk = [k for k in range(len(re)) if k not in artifact]   # 可信(弧)点
    axc.plot([re[k] for k in tk], [im_c[k] for k in tk], 'o-', color='#1f6feb', ms=4, lw=1.4,
             label='cell (trusted)')
    if artifact:
        axc.plot([re[k] for k in artifact], [im_c[k] for k in artifact], 'x', color='#8b949e',
                 ms=6, mew=1.4, label='HF artifact (trimmed)')
        axc.legend(loc='upper left', fontsize=8)
    if circle is not None:                    # 画拟合的 Rct 半圆 + 两个实轴截距
        xc, yc, R = circle
        th = np.linspace(0, math.pi, 200)
        cx, cy = xc + R * np.cos(th), yc + R * np.sin(th)
        m = cy >= -1
        axc.plot(cx[m], cy[m], '--', color='#cf222e', lw=1.0, alpha=0.7, label='Rct circle fit')
        axc.plot([Rs + Rct], [0], 'rD', ms=6)
        axc.annotate('Rs+Rct=%.0f' % (Rs + Rct), (Rs + Rct, 0), color='#cf222e', fontsize=8,
                     textcoords='offset points', xytext=(2, -14))
        # Rct 跨距标注 (Rs ↔ Rs+Rct, 实轴上方双箭头)
        ya = (yc + R) * 0.55
        axc.annotate('', xy=(Rs, ya), xytext=(Rs + Rct, ya),
                     arrowprops=dict(arrowstyle='<->', color='#7d4cdb', lw=1.6))
        axc.text((Rs + Rs + Rct) / 2, ya + 4, 'Rct = %.0f uOhm' % Rct, color='#7d4cdb',
                 fontsize=10, ha='center', fontweight='bold')
        axc.legend(loc='upper right', fontsize=8)
    axc.plot([Rs], [0], 'r*', ms=16)
    axc.axhline(0, color='#bbb', lw=0.6); axc.grid(True, alpha=0.3)
    axc.set_xlabel("Z' (uOhm)"); axc.set_ylabel("-Z'' (uOhm)")
    axc.set_title('L-COMPENSATED  ->  Rs/Rct by semicircle fit + Warburg')
    axc.annotate('Rs=%.0f' % Rs, (Rs, 0), color='#cf222e', fontsize=9,
                 textcoords='offset points', xytext=(-2, -14))
    hv = ('CV=%.1f%% %s' % (cv * 100, verdict_en) if cv is not None
          else (verdict_en if 'verdict_en' in dir() else 'ext short-cal'))
    rct_s = ('  Rct=%.0f uOhm' % Rct) if Rct else ''
    fig.suptitle('JCY8001 EIS   L=%.0f nH (%s)   Rs=%.3f mOhm%s' % (L_nH, hv, Rs / 1000, rct_s),
                 fontsize=11)
else:
    fig, ax = plt.subplots(figsize=(8.5, 6.2))
    ax.plot(re, im, 'o-', color='#1f77b4', ms=5, lw=1.3, label='measured (%d pts)' % len(re))
    ax.plot([Rs], [0], 'r*', ms=18, label='Rs=%.0f uOhm' % Rs)
    ax.axhline(0, color='gray', lw=0.6, ls='--'); ax.grid(True, alpha=0.3); ax.legend()
    ax.set_xlabel("Z_real (uOhm)"); ax.set_ylabel("-Z_imag (uOhm)")
    ax.set_title('JCY8001 EIS Nyquist  Rs=%.3f mOhm @ %.1f Hz' % (Rs / 1000, fz))

plt.tight_layout(rect=[0, 0, 1, 0.96]); plt.savefig(OUT, dpi=130)
print('saved', OUT)
