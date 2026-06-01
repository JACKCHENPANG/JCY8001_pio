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

// 固件自主扫频: 下发频点表 -> 触发 -> 轮询完成数边跑边取。
// npts: 点数(<=20); onPoint(idx,hz,re,im); 返回 {hz:[],re:[],im:[]}
async function runSweep(ble, opts) {
  opts = opts || {};
  const npts = Math.min(opts.npts || PTS.length, PTS.length);
  await setParams(ble, opts);
  // 下发频点表 (FC10 到 0x4400) + 点数
  const codes = PTS.slice(0, npts).map(p => p[0]);
  // 分批写 (FC10 一次最多 ~60 寄存器, 这里 <=20 直接一次)
  await ble.writeMulti(MB.REG.SWEEP_TABLE, codes);
  await ble.writeReg(MB.REG.SWEEP_COUNT, npts);
  await ble.writeCoil(MB.COIL.SWEEP_TRIG, 1);
  const hz = [], re = [], im = [];
  let got = 0;
  const t0 = Date.now();
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

module.exports = { PTS, readBasic, setParams, runSweep, sleep };
