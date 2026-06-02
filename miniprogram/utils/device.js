// JCY8001 高层操作 (基于 BleModbus + 寄存器图)。对齐 tools/eis_sweep_fw.py。
'use strict';
const MB = require('./modbus.js');

// 固件频率表有效点 (freqval=Exp<<8|Mant, Hz), 1kHz->0.119Hz, 与 PC 工具同
const PTS = [
  [0x0B42, 1007.083], [0x09A2, 617.983], [0x08C6, 377.656], [0x087A, 232.697], [0x0796, 143.052],
  [0x082E, 87.738], [0x073A, 55.313], [0x0812, 34.332], [0x0716, 20.981], [0x070E, 13.351],
  [0x0806, 11.444], [0x070A, 9.537], [0x0902, 7.629], [0x0706, 5.722], [0x0802, 3.815],
  [0x0702, 1.907], [0x0602, 0.954], [0x0502, 0.477], [0x0402, 0.238], [0x0302, 0.119],
];

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// 读基础信息
async function readBasic(ble) {
  const out = {};
  let r = await ble.readHolding(MB.REG.FW_VER, 1); if (r.ok) out.fw = r.regs[0];
  r = await ble.readHolding(MB.REG.TEMP, 1); if (r.ok) out.temp = r.regs[0] / 10;
  r = await ble.readHolding(MB.REG.VOLT, 2); if (r.ok) out.volt = r.regs[0] / 10000;
  r = await ble.readHolding(MB.REG.CH_COUNT, 1); if (r.ok) out.ch = r.regs[0];
  return out;
}

// 设测量参数
async function setParams(ble, p) {
  p = p || {};
  if (p.samp !== undefined) await ble.writeReg(MB.REG.SAMP_RES, p.samp);   // 0=10Ω 1=5Ω 2=1Ω
  if (p.gain !== undefined) await ble.writeReg(MB.REG.ZM_GAIN, p.gain);
  if (p.avg !== undefined) await ble.writeReg(MB.REG.AVG, p.avg);
  if (p.fast !== undefined) await ble.writeReg(MB.REG.ZM_FAST, p.fast);
  if (p.mode !== undefined) await ble.writeReg(MB.REG.ZM_MODE, p.mode);
}

// 单点 ZM: 设频率 -> 触发线圈 0x0000 -> 等完成 -> 读 RE/IM(μΩ 64位) + VZM(mV)
async function singleZM(ble, freqCode, timeoutMs) {
  timeoutMs = timeoutMs || 25000;
  await ble.writeReg(MB.REG.ZM_FREQ, freqCode);
  await ble.writeCoil(MB.COIL.ZM_TRIG, 0); await sleep(120);
  await ble.writeCoil(MB.COIL.ZM_TRIG, 1);
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    await sleep(300);
    const st = await ble.readHolding(MB.REG.STATUS, 1);
    if (st.ok && st.regs[0] === 0x0006) break;   // 测量完成
  }
  const rr = await ble.readHolding(MB.REG.RE_BASE, 4);
  const ri = await ble.readHolding(MB.REG.IM_BASE, 4);
  const vz = await ble.readHolding(MB.REG.VZM, 1);
  const re = rr.ok ? MB.regs64ToUOhm(rr.regs) : 0;     // μΩ
  const im = ri.ok ? MB.regs64ToUOhm(ri.regs) : 0;
  const vzm = vz.ok ? (vz.regs[0] * 4800 / 16383 + 1200) : null;  // mV
  return { re, im, vzm, mag: Math.hypot(re, im) };
}

// 自动选采样电阻档位: 先在 10Ω(最温和)探一点中频, 按"激励扰动电压"选档。
// 扰动 = I·|Z| = (VZM/R)·|Z|, 目标 ≤ ~15mV(EIS 小信号)且尽量大电流提 SNR。
// 返回 {sel, R, mag_uohm, pertV, note}。sel: 0=10Ω 1=5Ω 2=1Ω。
async function autoRange(ble, opts) {
  opts = opts || {};
  const probeFreq = opts.probeFreq || 0x082E;       // ~87.7Hz 中频
  const target = opts.targetPertV || 0.015;         // 15 mV
  const GEARS = [{ sel: 2, R: 1 }, { sel: 1, R: 5 }, { sel: 0, R: 10 }];
  await ble.writeReg(MB.REG.SAMP_RES, 0);           // 先 10Ω 探测(安全, 不过驱)
  await ble.writeReg(MB.REG.ZM_FAST, 1);
  const z = await singleZM(ble, probeFreq);
  const magOhm = z.mag * 1e-6;
  const vzmV = (z.vzm != null ? z.vzm : 3500) / 1000;
  // 探测失败/无效(|Z|≈0) → 默认 10Ω(最安全, 不过驱)
  if (!(magOhm > 0)) {
    return { sel: 0, R: 10, mag_uohm: 0, vzm_mV: z.vzm, pertV: 0,
             current_A: vzmV / 10, note: '探测失败, 默认 10Ω' };
  }
  // 选满足扰动≤target 的最小 R(电流最大/SNR最好); 都不满足→10Ω并告警
  let pick = GEARS[GEARS.length - 1], warn = '';
  for (const g of GEARS) {
    const pert = (vzmV / g.R) * magOhm;
    if (pert <= target) { pick = g; break; }
  }
  const pertPick = (vzmV / pick.R) * magOhm;
  if (pertPick > target) warn = '内阻偏高, 10Ω档仍过驱(需更大采样电阻硬件)';
  return { sel: pick.sel, R: pick.R, mag_uohm: z.mag, vzm_mV: z.vzm,
           pertV: pertPick, current_A: vzmV / pick.R, note: warn };
}

// 固件自主扫频: 下发频点表 -> 触发 -> 轮询完成数边跑边取。
// npts: 点数(<=20); onPoint(idx,hz,re,im); 返回 {hz:[],re:[],im:[]}
async function runSweep(ble, opts) {
  opts = opts || {};
  const npts = Math.min(opts.npts || PTS.length, PTS.length);
  // 自动挡: 先探测内阻按扰动选采样电阻档
  if (opts.auto) {
    const ar = await autoRange(ble, opts);
    opts.samp = ar.sel;
    if (opts.onAutoRange) opts.onAutoRange(ar);
  }
  await setParams(ble, opts);
  // 下发频点表 (FC10 到 0x4400) + 点数
  const codes = PTS.slice(0, npts).map(p => p[0]);
  // 分批写 (FC10 一次最多 ~60 寄存器, 这里 <=20 直接一次)
  await ble.writeMulti(MB.REG.SWEEP_TABLE, codes);
  await ble.writeReg(MB.REG.SWEEP_COUNT, npts);
  // 触发: 固件靠线圈上升沿启动, 必须先置 0 再置 1 制造沿(否则连测时第2次起不重扫, 读到残留)
  await ble.writeCoil(MB.COIL.SWEEP_TRIG, 0);
  await sleep(150);
  await ble.writeCoil(MB.COIL.SWEEP_TRIG, 1);
  const hz = [], re = [], im = [];
  let got = 0;
  const t0 = Date.now();
  // 等"已完成数"复位 (新扫频确实开始), 避免读上一次残留
  for (let w = 0; w < 40; w++) {
    const dr = await ble.readHolding(MB.REG.SWEEP_DONE, 1);
    if (dr.ok && dr.regs[0] < npts) break;
    await sleep(200);
  }
  while (got < npts) {
    await sleep(300);
    const d = await ble.readHolding(MB.REG.SWEEP_DONE, 1);
    const done = d.ok ? d.regs[0] : got;
    while (got < done && got < npts) {
      const rr = await ble.readHolding(MB.REG.SWEEP_RE + got * 4, 4);
      const ri = await ble.readHolding(MB.REG.SWEEP_IM + got * 4, 4);
      const reU = rr.ok ? MB.regs64ToUOhm(rr.regs) : 0;
      const imU = ri.ok ? MB.regs64ToUOhm(ri.regs) : 0;
      hz.push(PTS[got][1]); re.push(reU); im.push(imU);
      if (opts.onPoint) opts.onPoint(got, PTS[got][1], reU, imU);
      got++;
    }
    if (Date.now() - t0 > 180000) break;     // 3min 超时保护
  }
  return { hz, re, im };
}

module.exports = { PTS, readBasic, setParams, runSweep, singleZM, autoRange, sleep };
