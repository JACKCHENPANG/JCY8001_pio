#!/usr/bin/env python3
"""DRT 弛豫时间分布 — 把 EIS 谱反卷积成 γ(τ) 峰, 分离重叠的极化过程 (SEI / 电荷转移 / 扩散)。

模型:  Z(ω) = Rs + Σ_m γ_m / (1 + jωτ_m)     (串联电感先扣掉)
解法:  非负 Tikhonov: min ||A x − b||² + λ²||D x||² , x≥0  (scipy NNLS, D=2阶差分平滑)
每个 γ(τ) 峰 = 一个极化过程; 峰面积(∫γ dlnτ) = 该过程电阻。峰位 f=1/(2πτ)。

用法: python3 eis_drt.py eis.csv [out.png] [--lam 1e-2] [--nt 70]
约定: CSV 的 IM 已是 -Z''。自动拟合并扣串联电感 L + 剔高频互感伪迹后再做 DRT。
依赖: numpy scipy (画图 matplotlib)。

⚠️ 要分出 R_SEI 必须高频干净(短 Kelvin 引线 + 频点密); 高频被夹具电感/互感伪迹占时,
   SEI 峰出不来, 只能看到 Rct/扩散。本工具先备用, 夹具搞干净再用。
"""
import sys, csv, math
import numpy as np
from scipy.optimize import nnls

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = sys.argv[1:]
CSV = args[0] if len(args) > 0 else 'eis.csv'
OUT = args[1] if len(args) > 1 else None
def opt(name, d):
    return float(opts[opts.index(name) + 1]) if name in opts and opts.index(name) + 1 < len(opts) else d
LAM = opt('--lam', 1e-2)
NT = int(opt('--nt', 70))

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

# 1) 拟合并扣串联电感 L (高频 Z''≈ωL)
hf = hz >= 300
L = float(np.sum((-im[hf] * 1e-6) * w[hf]) / np.sum(w[hf] ** 2)) if hf.sum() >= 2 else 0.0
# 2) 剔高频互感伪迹 (实部 Z'min 以上)
iMin = int(np.argmin(re))
keep = np.arange(iMin, len(hz))
wf = w[keep]
Zr = re[keep] * 1e-6                                 # Ω
Zi = (-im[keep]) * 1e-6 - wf * L                     # Ω, 扣 jωL 后的虚部 (Z'' = -im)
print('L=%.0f nH (已扣), 剔高频伪迹 %d 点, DRT 用 %d 点, λ=%.1e, Nτ=%d'
      % (L * 1e9, iMin, len(keep), LAM, NT))

# 3) τ 网格 (log, 略超数据频率范围)
fmin, fmax = hz[keep].min(), hz[keep].max()
tmin = 1.0 / (2 * np.pi * fmax) / 5
tmax = 5.0 / (2 * np.pi * fmin)
lnt = np.linspace(math.log(tmin), math.log(tmax), NT)
tau = np.exp(lnt)

# 4) 核矩阵 A: 未知 = [γ_0..γ_{NT-1}, Rs]
N = len(keep)
A = np.zeros((2 * N, NT + 1))
b = np.zeros(2 * N)
for i in range(N):
    wt = wf[i] * tau                                 # ωτ
    A[i, :NT] = 1.0 / (1.0 + wt ** 2)                # Re[1/(1+jωτ)]
    A[i, NT] = 1.0                                   # Rs (实部)
    b[i] = Zr[i]
    A[N + i, :NT] = -wt / (1.0 + wt ** 2)            # Im[1/(1+jωτ)]
    b[N + i] = Zi[i]

# 5) 2阶差分正则 (只作用于 γ, 不动 Rs)
D = np.zeros((NT, NT + 1))
for m in range(1, NT - 1):
    D[m, m - 1] = 1; D[m, m] = -2; D[m, m + 1] = 1
Aaug = np.vstack([A, math.sqrt(LAM) * D])
baug = np.concatenate([b, np.zeros(NT)])

x, _ = nnls(Aaug, baug)
gamma = x[:NT]; Rs = x[NT]
# A 未含 dlnt → γ_m 即各 τ 点的电阻贡献, 极化电阻直接 Σγ (DC: Z'=Rs+Σγ)
Rpol_total = float(np.sum(gamma)) * 1e6               # μΩ
print('Rs ≈ %.0f μΩ; 总极化电阻 ΣRpol ≈ %.0f μΩ' % (Rs * 1e6, Rpol_total))

# 找峰 (局部极大) + 峰面积≈该过程电阻
peaks = []
for m in range(1, NT - 1):
    if gamma[m] > gamma[m - 1] and gamma[m] >= gamma[m + 1] and gamma[m] > 0.02 * gamma.max():
        f_pk = 1.0 / (2 * np.pi * tau[m])
        # 峰局部面积 (谷到谷)
        lo = m
        while lo > 0 and gamma[lo - 1] <= gamma[lo]:
            lo -= 1
        hi = m
        while hi < NT - 1 and gamma[hi + 1] <= gamma[hi]:
            hi += 1
        R_pk = float(np.sum(gamma[lo:hi + 1])) * 1e6
        peaks.append((f_pk, R_pk))
print('DRT 峰 (高频→低频):')
for f_pk, R_pk in sorted(peaks, key=lambda p: -p[0]):
    print('   f≈%-9.3g Hz   R≈%.0f μΩ' % (f_pk, R_pk))

if OUT:
    import matplotlib; matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    f_axis = 1.0 / (2 * np.pi * tau)
    fig, ax = plt.subplots(figsize=(9, 5))
    ax.semilogx(f_axis, gamma * 1e6, '-', color='#1f6feb', lw=1.8)
    ax.fill_between(f_axis, gamma * 1e6, alpha=0.15, color='#1f6feb')
    for f_pk, R_pk in peaks:
        ax.axvline(f_pk, color='#cf222e', ls=':', lw=0.8)
        ax.annotate('%.0fμΩ\n@%.3gHz' % (R_pk, f_pk), (f_pk, gamma.max() * 1e6 * 0.9),
                    fontsize=8, ha='center', color='#cf222e')
    ax.set_xlabel('frequency 1/(2πτ)  (Hz)'); ax.set_ylabel('γ(τ)  (μΩ)')
    ax.set_title('JCY8001 DRT  (Rs=%.0f uOhm, sum Rpol=%.0f uOhm, L-removed=%.0f nH, lam=%.0e)'
                 % (Rs * 1e6, Rpol_total, L * 1e9, LAM))
    ax.set_ylabel('gamma(tau)  (uOhm)')
    ax.grid(True, alpha=0.3, which='both')
    plt.tight_layout(); plt.savefig(OUT, dpi=130); print('saved', OUT)
