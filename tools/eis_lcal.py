#!/usr/bin/env python3
"""夹具串联电感短路标定 (得到"真值"L, 给 eis_nyquist.py --lcal 用)。

做法: 把测试夹短接(不接电芯, 夹一段低感铜排/短路片), 用 eis_sweep_fw.py
扫一遍 → 得到 CSV。此时测到的虚部几乎全是夹具电感, 用全部(或高频)点拟合
L = sum(Z''*w)/sum(w^2), 比"带电芯自动拟合"更纯(无电芯高频电抗偏置)。

用法:
  1) python3 eis_sweep_fw.py /dev/ttyUSB0 short.csv     # 夹子短接后扫
  2) python3 eis_lcal.py short.csv [lcal.txt] [--hfmin 0]
     输出 L(nH) 到 lcal.txt; 之后:
       python3 eis_nyquist.py cell.csv out.png --lcal lcal.txt

短接 CSV 里 RE 应接近 0(纯引线电阻), 若 RE 偏大说明短路件本身有阻/接触差。
"""
import sys, csv, math

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = sys.argv[1:]
CSV = args[0] if len(args) > 0 else 'short.csv'
OUT = args[1] if len(args) > 1 else 'lcal.txt'
def opt(name, d):
    return opts[opts.index(name) + 1] if name in opts and opts.index(name) + 1 < len(opts) else d
HF_MIN = float(opt('--hfmin', '0'))   # 短接谱整段都可用, 默认全用

hz, re, im = [], [], []
for row in csv.DictReader(open(CSV)):
    try:
        h = float(row['Hz']); r = float(row['RE_uOhm']); i = float(row['IM_uOhm'])
    except (ValueError, KeyError):
        continue
    if r != r or i != i:
        continue
    hz.append(h); re.append(r); im.append(i)

pts = [(h, -i) for h, i in zip(hz, im) if h >= HF_MIN]   # (f, Z''=-IM, uOhm)
if len(pts) < 2:
    print('错误: 有效点不足2个'); sys.exit(1)

num = sum((zpp * 1e-6) * (2 * math.pi * f) for f, zpp in pts)
den = sum((2 * math.pi * f) ** 2 for f, _ in pts)
L = num / den                       # H
Li = [(zpp * 1e-6) / (2 * math.pi * f) for f, zpp in pts]
mean = sum(Li) / len(Li)
sd = (sum((x - mean) ** 2 for x in Li) / len(Li)) ** 0.5
cv = sd / mean if mean else 0
re_mean = sum(re) / len(re)

print('短路标定: L = %.1f nH  (%d点, CV=%.1f%%)' % (L * 1e9, len(pts), cv * 100))
print('短接残余实部 RE 均值 = %.1f uOhm (越小越好, 偏大=短路件有阻/接触差)' % re_mean)
if cv >= 0.10:
    print('⚠ CV>10%: 短接谱不够纯, 检查短路件低感+接触, 或缩小 hfmin 用高频段')
open(OUT, 'w').write('%.3f  # nH, fixture series inductance (short-circuit cal)\n' % (L * 1e9))
print('写入', OUT)
