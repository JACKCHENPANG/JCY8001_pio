#!/usr/bin/env python3
"""JCY8001 EIS 布线/接触补偿 (AN2001).
补偿模型(对每个频率的实部/虚部加一阶修正):
  Re_comp = Re_meas + Mre*f + Rpar
  Im_comp = Im_meas + Mim*f + Ipar
4参数 Mre/Rpar/Mim/Ipar 用最小二乘从"参考件"拟合(参考件真值已知: 短路件=0, 或精密电阻, 或电化学工作站测的真值)。

用法:
  # 1. 校准: 用参考件扫频CSV(freq_hz,RE_uohm,IM_uohm) + 参考真值 拟合4参数
  python3 eis_wiring_comp.py cal ref_sweep.csv --re-true 0 --im-true 0 [-o wiring.json]
  #    (短路件: re/im真值=0; 精密电阻Rref: --re-true Rref_uohm --im-true 0;
  #     电化学工作站: 提供逐频真值文件 --truth truth.csv[freq,RE,IM])
  # 2. 应用: 把补偿参数减到被测扫频上
  python3 eis_wiring_comp.py apply meas_sweep.csv wiring.json [-o meas_comp.csv]
"""
import sys, csv, json

def load(fn):
    rows=[]
    for r in csv.reader(open(fn)):
        if not r or r[0].startswith('#') or not r[0].replace('.','').replace('-','').isdigit():
            try: float(r[0])
            except: continue
        rows.append((float(r[0]),float(r[1]),float(r[2])))
    return rows

def lsq_line(xs, ys):
    """最小二乘 y = m*x + b, 返回(m,b)."""
    n=len(xs); sx=sum(xs); sy=sum(ys); sxx=sum(x*x for x in xs); sxy=sum(x*y for x,y in zip(xs,ys))
    d=n*sxx-sx*sx
    if abs(d)<1e-30: return 0.0, sy/n
    m=(n*sxy-sx*sy)/d; b=(sy-m*sx)/n
    return m,b

def cmd_cal(args):
    ref=load(args[0])
    re_true=im_true=0.0; truth=None; out="wiring.json"
    i=1
    while i<len(args):
        if args[i]=="--re-true": re_true=float(args[i+1]); i+=2
        elif args[i]=="--im-true": im_true=float(args[i+1]); i+=2
        elif args[i]=="--truth": truth={f:(r,m) for f,r,m in load(args[i+1])}; i+=2
        elif args[i]=="-o": out=args[i+1]; i+=2
        else: i+=1
    fs=[f for f,_,_ in ref]
    # 残差 = 真值 - 实测 ; 补偿要 Re_meas+Mre*f+Rpar=真值 → (真值-Re_meas)=Mre*f+Rpar
    re_resid=[ (truth[f][0] if truth else re_true) - r for f,r,_ in ref ]
    im_resid=[ (truth[f][1] if truth else im_true) - m for f,_,m in ref ]
    Mre,Rpar=lsq_line(fs,re_resid); Mim,Ipar=lsq_line(fs,im_resid)
    p={"Mre":Mre,"Rpar":Rpar,"Mim":Mim,"Ipar":Ipar,"note":"Re_comp=Re+Mre*f+Rpar; Im_comp=Im+Mim*f+Ipar (uohm, f in Hz)"}
    json.dump(p,open(out,"w"),indent=2)
    print("布线补偿参数(AN2001):")
    print("  Mre=%.6g uohm/Hz  Rpar=%.3f uohm"%(Mre,Rpar))
    print("  Mim=%.6g uohm/Hz  Ipar=%.3f uohm"%(Mim,Ipar))
    # 残差检查
    res_re=[abs((truth[f][0] if truth else re_true)-(r+Mre*f+Rpar)) for f,r,_ in ref]
    print("  拟合后实部最大残差 %.2f uohm"%max(res_re))
    print("写入",out)

def cmd_apply(args):
    meas=load(args[0]); p=json.load(open(args[1])); out="meas_comp.csv"
    if "-o" in args: out=args[args.index("-o")+1]
    w=csv.writer(open(out,"w"))
    w.writerow(["freq_hz","RE_uohm","IM_uohm","mag_uohm"])
    for f,r,m in meas:
        rc=r+p["Mre"]*f+p["Rpar"]; ic=m+p["Mim"]*f+p["Ipar"]
        w.writerow(["%.4f"%f,"%.2f"%rc,"%.2f"%ic,"%.2f"%((rc*rc+ic*ic)**0.5)])
    print("补偿后写入",out)

if __name__=="__main__":
    if len(sys.argv)<3 or sys.argv[1] not in("cal","apply"): print(__doc__); sys.exit(1)
    (cmd_cal if sys.argv[1]=="cal" else cmd_apply)(sys.argv[2:])
