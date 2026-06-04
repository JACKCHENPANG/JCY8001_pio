// EIS 分析 (微信小程序 / Node 通用纯 JS)
// 移植自 tools/eis_nyquist.py + tools/eis_ecm.py。
// 约定: re/im 单位 μΩ, im 已是 -Z'' (Z = re + j*(-im))。

'use strict';

// ── 夹具串联电感 L 拟合 (高频 Z''≈ωL, 最小二乘过原点) ──
function fitInductance(hz, re, im, hfmin) {
  hfmin = hfmin || 300;
  let num = 0, den = 0;
  const Li = [];
  for (let k = 0; k < hz.length; k++) {
    if (hz[k] < hfmin) continue;
    const w = 2 * Math.PI * hz[k];
    const zpp = -im[k] * 1e-6;          // Z'' in Ω
    num += zpp * w; den += w * w;
    Li.push(zpp / w);
  }
  if (Li.length < 2) return { L: null, cv: null, n: Li.length };
  const L = num / den;                  // H
  const mean = Li.reduce((a, b) => a + b, 0) / Li.length;
  const sd = Math.sqrt(Li.reduce((a, b) => a + (b - mean) * (b - mean), 0) / Li.length);
  return { L: L, L_nH: L * 1e9, cv: mean ? sd / mean : 0, n: Li.length };
}

// ── 高斯消元解 n×n ──
function solveLin(A, b) {
  const n = b.length;
  const M = A.map((row, i) => row.slice().concat([b[i]]));
  for (let c = 0; c < n; c++) {
    let p = c;
    for (let r = c + 1; r < n; r++) if (Math.abs(M[r][c]) > Math.abs(M[p][c])) p = r;
    if (Math.abs(M[p][c]) < 1e-15) return null;
    [M[c], M[p]] = [M[p], M[c]];
    for (let r = 0; r < n; r++) {
      if (r === c) continue;
      const f = M[r][c] / M[c][c];
      for (let k = c; k <= n; k++) M[r][k] -= f * M[c][k];
    }
  }
  return M.map((row, i) => row[n] / row[i]);
}

// ── 找实部最低点 (Z'min = 高频伪迹边界, 之上的点为互感伪迹) ──
function findArtifact(re) {
  let iMin = 0;
  for (let k = 1; k < re.length; k++) if (re[k] < re[iMin]) iMin = k;
  return iMin;                          // 列表按 HF->LF, [0,iMin) = 伪迹
}

// ── Rct 半圆 Kasa 圆拟合, 返回 Rs(左截距)/Rct(直径) ──
function semicircleFit(hz, re, imc, iMin) {
  const trusted = [];
  for (let k = iMin; k < re.length; k++) trusted.push(k);
  if (trusted.length < 4) return { Rs: re[iMin], Rct: null, circle: null };
  // Rct 弧峰 = 中频 imc 局部极大; 限定 hz>=5 避开低频 Warburg 上翘尖
  const cand = trusted.filter(k => hz[k] >= 5);
  const pool = cand.length ? cand : trusted;
  let peak = pool[0];
  for (const k of pool) if (imc[k] > imc[peak]) peak = k;
  // 峰后第一个谷 = Warburg 起点
  let we = re.length - 1;
  for (let k = peak; k < re.length - 1; k++) { if (imc[k + 1] > imc[k]) { we = k; break; } }
  const arc = [];
  for (let k = iMin; k <= we; k++) arc.push(k);
  if (arc.length < 4) return { Rs: re[iMin], Rct: null, circle: null };
  // Kasa: x²+y²+Dx+Ey+F=0
  let sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sxz = 0, syz = 0, sz = 0;
  const m = arc.length;
  for (const k of arc) {
    const x = re[k], y = imc[k], z = x * x + y * y;
    sxx += x * x; sxy += x * y; syy += y * y; sx += x; sy += y;
    sxz += x * z; syz += y * z; sz += z;
  }
  const sol = solveLin([[sxx, sxy, sx], [sxy, syy, sy], [sx, sy, m]], [-sxz, -syz, -sz]);
  if (!sol) return { Rs: re[iMin], Rct: null, circle: null };
  const [D, E, F] = sol;
  const disc = D * D - 4 * F;
  if (disc <= 0) return { Rs: re[iMin], Rct: null, circle: null };
  const r1 = (-D - Math.sqrt(disc)) / 2, r2 = (-D + Math.sqrt(disc)) / 2;
  const Rs = Math.min(r1, r2), Rsum = Math.max(r1, r2);
  const xc = -D / 2, yc = -E / 2;
  return { Rs: Rs, Rct: Rsum - Rs, circle: { xc: xc, yc: yc, R: Math.sqrt(xc * xc + yc * yc - F) } };
}

// ── 完整分析: L + 补偿 + 伪迹 + Rs/Rct ──
function analyze(hz, re, im, opts) {
  opts = opts || {};
  const out = { L_nH: null, cv: null, imc: im.slice(), artifact: [], Rs: null, Rct: null, circle: null };
  if (hz.length < 4) return out;
  const f = fitInductance(hz, re, im, opts.hfmin);
  if (f.L == null) return out;
  // L≤0 = 高频无感性尾(高内阻电芯, 电感可忽略) → 不补偿
  const L = f.L > 0 ? f.L : 0;
  out.L_nH = f.L > 0 ? f.L_nH : 0; out.cv = f.L > 0 ? f.cv : null;
  const imc = hz.map((h, k) => im[k] + 2 * Math.PI * h * L * 1e6);
  out.imc = imc;
  const iMin = findArtifact(re);
  for (let k = 0; k < iMin; k++) out.artifact.push(k);
  const sc = semicircleFit(hz, re, imc, iMin);
  out.Rs = sc.Rs; out.Rct = sc.Rct; out.circle = sc.circle;
  return out;
}

// ── 复数辅助 ──
function cAdd(a, b) { return [a[0] + b[0], a[1] + b[1]]; }
function cMul(a, b) { return [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]]; }
function cInv(a) { const d = a[0] * a[0] + a[1] * a[1]; return [a[0] / d, -a[1] / d]; }
// (jω)^n = ω^n (cos(nπ/2) + j sin(nπ/2))
function jwPow(w, n) { const m = Math.pow(w, n), t = n * Math.PI / 2; return [m * Math.cos(t), m * Math.sin(t)]; }

// ECM 模型 Z(ω) = jωL + Rs + [CPE ‖ (Rct + Zw)]   (SI: Ω,H,F)
function ecmModel(p, w) {
  const L = p[0], Rs = p[1], Rct = p[2], Q = p[3], n = p[4], sig = p[5];
  const jwn = jwPow(w, n);                          // (jω)^n
  const Ycpe = [Q * jwn[0], Q * jwn[1]];            // Y_CPE = Q(jω)^n
  const jwm = jwPow(w, -0.5);
  const Zw = [sig * jwm[0], sig * jwm[1]];          // Warburg
  const Zrw = [Rct + Zw[0], Zw[1]];
  const Yp = cAdd(Ycpe, cInv(Zrw));                 // CPE ‖ (Rct+Zw) 的导纳
  const Zp = cInv(Yp);
  return cAdd([Rs, w * L], Zp);                     // jωL 的实部 0, 虚部 ωL
}

// ── ECM 拟合: Levenberg-Marquardt, 相对残差, 剔高频伪迹后拟合 ──
function ecmFit(hz, re, im) {
  const iMin = findArtifact(re);
  const idx = [];
  for (let k = iMin; k < hz.length; k++) idx.push(k);
  if (idx.length < 6) return null;
  const w = idx.map(k => 2 * Math.PI * hz[k]);
  const Zr = idx.map(k => re[k] * 1e-6);            // Ω
  const Zi = idx.map(k => -im[k] * 1e-6);           // Z'' = -im
  const mag = idx.map((_, j) => Math.hypot(Zr[j], Zi[j]) || 1e-12);

  function residuals(p) {
    const r = [];
    for (let j = 0; j < w.length; j++) {
      const Zm = ecmModel(p, w[j]);
      r.push((Zm[0] - Zr[j]) / mag[j]);
      r.push((Zm[1] - Zi[j]) / mag[j]);
    }
    return r;
  }
  function cost(r) { return r.reduce((a, b) => a + b * b, 0); }

  // 初值 + 边界
  // 上界按电芯量级自适应 (亚mΩ 低阻 ~ 几十mΩ 高阻三元都覆盖)
  const Rmax = Math.max(re[re.length - 1] * 1e-6 * 5, 1e-2);
  const Rs0 = Math.min(re[iMin] * 1e-6, Rmax * 0.9);
  const Rct0 = Math.min(Math.max((re[re.length - 1] - re[iMin]) * 1e-6 * 0.8, 1e-5), Rmax * 0.9);
  let p = [2e-7, Rs0, Rct0, 10.0, 0.85, 5e-5];
  const lb = [1e-9, 1e-6, 1e-6, 1e-6, 0.3, 0.0];
  const ub = [1e-6, Rmax, Rmax, 1e6, 1.0, 1e0];
  const clamp = q => q.map((v, i) => Math.min(Math.max(v, lb[i]), ub[i]));
  p = clamp(p);

  let lambda = 1e-3;
  let r = residuals(p), c = cost(r);
  const N = p.length;
  for (let iter = 0; iter < 120; iter++) {
    // 数值 Jacobian
    const J = r.map(() => new Array(N).fill(0));
    for (let i = 0; i < N; i++) {
      const dp = Math.max(Math.abs(p[i]) * 1e-6, 1e-12);
      const pp = p.slice(); pp[i] += dp;
      const rp = residuals(pp);
      for (let k = 0; k < r.length; k++) J[k][i] = (rp[k] - r[k]) / dp;
    }
    // JtJ, Jtr
    const JtJ = Array.from({ length: N }, () => new Array(N).fill(0));
    const Jtr = new Array(N).fill(0);
    for (let k = 0; k < r.length; k++) {
      for (let i = 0; i < N; i++) {
        Jtr[i] += J[k][i] * r[k];
        for (let j = 0; j < N; j++) JtJ[i][j] += J[k][i] * J[k][j];
      }
    }
    let improved = false;
    for (let tryi = 0; tryi < 8; tryi++) {
      const A = JtJ.map((row, i) => row.map((v, j) => (i === j ? v * (1 + lambda) : v)));
      const dp = solveLin(A, Jtr.map(v => -v));
      if (!dp) { lambda *= 10; continue; }
      const pn = clamp(p.map((v, i) => v + dp[i]));
      const rn = residuals(pn), cn = cost(rn);
      if (cn < c) { p = pn; r = rn; c = cn; lambda = Math.max(lambda / 3, 1e-9); improved = true; break; }
      lambda *= 10;
    }
    if (!improved && lambda > 1e7) break;
  }
  const rms = Math.sqrt(2 * c / r.length) * 100;   // 每点复数 (r.length=2N), 与 PC 工具同口径
  const [L, Rs, Rct, Q, n, sig] = p;
  const Cdl = Math.pow(Q * Math.pow(1 / Rs + 1 / Rct, n - 1), 1 / n);
  return {
    L: L, L_nH: L * 1e9, Rs: Rs, Rs_uOhm: Rs * 1e6, Rct: Rct, Rct_uOhm: Rct * 1e6,
    Q: Q, n: n, sig: sig, Cdl: Cdl, rms: rms, iMin: iMin
  };
}

// ── 整包一致性: 以 Rct 为主, 算中位数+偏差, 标超差 ──
function consistency(rcts, tolPct) {
  const sorted = rcts.slice().sort((a, b) => a - b);
  const med = sorted.length ? sorted[Math.floor(sorted.length / 2)] : 0;
  const dev = rcts.map(v => (med ? (v - med) / med * 100 : 0));
  const bad = [];
  dev.forEach((d, i) => { if (Math.abs(d) > tolPct) bad.push({ i: i, dev: d }); });
  return { median: med, dev: dev, bad: bad };
}

// ════════════ SOC / 温度 归一化 (Phase 1: 文献默认曲线, 标"待标定") ════════════
// 静置 OCV(V) → 化学体系分桶。粗判, 拆机混料用。
function detectChemistry(ocv) {
  if (ocv == null || !isFinite(ocv)) return { chem: 'unknown', name: '未知' };
  if (ocv >= 2.0 && ocv < 2.9) return { chem: 'LTO', name: '钛酸锂' };
  if (ocv >= 2.9 && ocv <= 3.45) return { chem: 'LFP', name: '磷酸铁锂' };
  if (ocv > 3.45 && ocv <= 4.3) return { chem: 'NMC', name: '三元/钴酸锂' };
  return { chem: 'unknown', name: '未知' };
}

// 文献典型 OCV-SOC 表 (单调插值)。LFP 平台段 SOC 不可靠(reliable=false)。
const OCV_SOC = {
  LFP: { soc: [0, 5, 10, 20, 50, 80, 90, 95, 100], ocv: [2.50, 3.00, 3.20, 3.25, 3.30, 3.32, 3.34, 3.45, 3.65] },
  NMC: { soc: [0, 10, 20, 50, 80, 90, 100], ocv: [3.00, 3.50, 3.60, 3.70, 4.00, 4.10, 4.20] },
  LTO: { soc: [0, 10, 50, 90, 100], ocv: [2.00, 2.20, 2.35, 2.50, 2.70] },
};
function interp(x, xs, ys) {
  if (x <= xs[0]) return ys[0];
  if (x >= xs[xs.length - 1]) return ys[ys.length - 1];
  for (let i = 1; i < xs.length; i++) if (x <= xs[i]) {
    const t = (x - xs[i - 1]) / (xs[i] - xs[i - 1]); return ys[i - 1] + t * (ys[i] - ys[i - 1]);
  }
  return ys[ys.length - 1];
}
function socFromOcv(ocv, chem) {
  const t = OCV_SOC[chem]; if (!t || ocv == null) return { soc: null, reliable: false };
  const soc = interp(ocv, t.ocv, t.soc);
  // LFP 平台 20~90% OCV 几乎不变 → SOC 不可靠
  const reliable = chem === 'LFP' ? (soc < 18 || soc > 92) : true;
  return { soc: Math.max(0, Math.min(100, soc)), reliable };
}

// 文献典型 Rct(SOC) 相对系数, 参考 SOC=50%。k = Rct@50% / Rct(SOC) (乘到实测把它拉到50%口径)
const KSOC = {
  LFP: { soc: [0, 10, 15, 20, 50, 80, 85, 90, 100], k: [0.70, 0.85, 0.95, 1.00, 1.00, 1.00, 0.95, 0.85, 0.70] },
  NMC: { soc: [0, 10, 20, 50, 80, 90, 100], k: [0.60, 0.80, 0.92, 1.00, 0.92, 0.80, 0.62] },
  LTO: { soc: [0, 20, 50, 80, 100], k: [0.85, 0.95, 1.00, 0.95, 0.85] },
};
function kSoc(soc, chem) { const t = KSOC[chem]; if (!t || soc == null) return 1; return interp(soc, t.soc, t.k); }

// Arrhenius 温度归一到 25℃: Rct(T)=A·exp(Ea/RT); k(T)=Rct(25)/Rct(T)=exp(Ea/R·(1/298.15-1/T))
function kTemp(tempC, Ea) {
  if (tempC == null || !isFinite(tempC)) return 1;
  Ea = Ea || 40000; const R = 8.314, T = tempC + 273.15, Tref = 298.15;
  return Math.exp(Ea / R * (1 / Tref - 1 / T));
}

// Rct(µΩ) + OCV(V) + temp(℃) → 归一化到 SOC50%/25℃。返回 chem/soc/归一值/置信度。
function normalizeRct(rct_uOhm, ocv, tempC, chemOverride) {
  const det = chemOverride ? { chem: chemOverride, name: chemOverride } : detectChemistry(ocv);
  const s = socFromOcv(ocv, det.chem);
  const ks = kSoc(s.soc, det.chem), kt = kTemp(tempC);
  const rct_norm = (rct_uOhm == null) ? null : rct_uOhm * ks * kt;
  // 置信度: SOC 不可靠 或 偏离中段太远 或 温度偏离大 → 低
  let conf = 'high';
  if (det.chem === 'unknown') conf = 'none';
  else if (!s.reliable) conf = 'low';
  else if (s.soc != null && (s.soc < 25 || s.soc > 75)) conf = 'mid';
  else if (tempC != null && Math.abs(tempC - 25) > 12) conf = 'mid';
  return {
    chem: det.chem, chemName: det.name, ocv: ocv, soc: s.soc, socReliable: s.reliable,
    kSoc: ks, kTemp: kt, rct_raw: rct_uOhm, rct_norm: rct_norm, conf: conf,
    note: '默认文献曲线, 待标定'
  };
}

const API = { fitInductance, analyze, semicircleFit, ecmFit, ecmModel, consistency, findArtifact,
  detectChemistry, socFromOcv, normalizeRct, kSoc, kTemp };
if (typeof module !== 'undefined' && module.exports) module.exports = API;   // Node 测试
// 小程序: const A = require('../../utils/analysis.js')
