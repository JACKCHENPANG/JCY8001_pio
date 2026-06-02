// Modbus RTU 编解码 (微信小程序 / Node 通用)。从机 0x01。
// 寄存器图与 JCY8001 固件一致 (src/main.c)。值仅做编解码, 不含传输 (BLE 在 ble.js)。
'use strict';

const SID = 0x01;

function crc16(bytes, len) {
  len = (len === undefined) ? bytes.length : len;
  let c = 0xFFFF;
  for (let i = 0; i < len; i++) {
    c ^= bytes[i];
    for (let b = 0; b < 8; b++) c = (c & 1) ? ((c >> 1) ^ 0xA001) : (c >> 1);
  }
  return c & 0xFFFF;
}

function withCrc(arr) {
  const c = crc16(arr, arr.length);
  return new Uint8Array(arr.concat([c & 0xFF, (c >> 8) & 0xFF]));
}

// FC03 读保持寄存器
function readHolding(reg, count) {
  return withCrc([SID, 0x03, (reg >> 8) & 0xFF, reg & 0xFF, (count >> 8) & 0xFF, count & 0xFF]);
}
// FC06 写单寄存器
function writeReg(reg, val) {
  return withCrc([SID, 0x06, (reg >> 8) & 0xFF, reg & 0xFF, (val >> 8) & 0xFF, val & 0xFF]);
}
// FC05 写线圈
function writeCoil(addr, on) {
  return withCrc([SID, 0x05, (addr >> 8) & 0xFF, addr & 0xFF, on ? 0xFF : 0x00, 0x00]);
}
// FC10 写多寄存器 (vals: 数组)
function writeMulti(reg, vals) {
  const n = vals.length;
  const a = [SID, 0x10, (reg >> 8) & 0xFF, reg & 0xFF, (n >> 8) & 0xFF, n & 0xFF, n * 2];
  for (const v of vals) { a.push((v >> 8) & 0xFF); a.push(v & 0xFF); }
  return withCrc(a);
}

// 解析 FC03/04 响应 -> 16位寄存器数组; 校验从机/功能码/CRC/字节数。
function parseRead(buf) {
  if (!buf || buf.length < 5) return { ok: false, err: 'short' };
  if (buf[0] !== SID) return { ok: false, err: 'sid' };
  if (buf[1] & 0x80) return { ok: false, err: 'exception', code: buf[2] };
  const nb = buf[2];
  if (buf.length < 3 + nb + 2) return { ok: false, err: 'len' };
  const crc = crc16(buf, 3 + nb);
  const got = buf[3 + nb] | (buf[3 + nb + 1] << 8);
  if (crc !== got) return { ok: false, err: 'crc' };
  const regs = [];
  for (let i = 0; i < nb; i += 2) regs.push((buf[3 + i] << 8) | buf[3 + i + 1]);
  return { ok: true, regs: regs };
}

// 4 个 16位寄存器 (大端) 拼成 64位有符号, /1e5 = μΩ
function regs64ToUOhm(regs, off) {
  off = off || 0;
  let v = 0n;
  for (let i = 0; i < 4; i++) v = (v << 16n) | BigInt(regs[off + i] & 0xFFFF);
  if (v >> 63n) v -= (1n << 64n);          // 有符号
  return Number(v) / 100000.0;
}

// 简单确认响应 (FC05/06/10): 至少 6 字节且功能码无异常
function parseAck(buf) {
  if (!buf || buf.length < 4) return false;
  return buf[0] === SID && !(buf[1] & 0x80);
}

// ── JCY8001 寄存器图 (src/main.c) ──
const REG = {
  TEMP: 0x3300, VOLT: 0x3340, STATUS: 0x3380,
  CH_COUNT: 0x3E00, FW_VER: 0x3E02, BUILD_DATE: 0x3E04,
  ZM_FREQ: 0x4200, SAMP_RES: 0x40C0, ZM_GAIN: 0x4280, AVG: 0x4040,
  ZM_FAST: 0x4340, ZM_MODE: 0x4300, AUTORANGE: 0x4380,
  RE_BASE: 0x3000, IM_BASE: 0x3080,          // 单点 64位 μΩ
  VZM: 0x3200,                                // 单点 VZM 原始 (raw*4800/16383+1200 mV)
  // 固件自主扫频
  SWEEP_TABLE: 0x4400, SWEEP_COUNT: 0x43C0,
  SWEEP_STATE: 0x3E40, SWEEP_DONE: 0x3E41,
  SWEEP_RE: 0x3400, SWEEP_IM: 0x3500, SWEEP_FREQ: 0x3640,   // +idx (RE/IM 每点 4 寄存器)
};
const COIL = { ZM_TRIG: 0x0000, SWEEP_TRIG: 0x00C0, BAL: 0x0040 };

module.exports = {
  SID, crc16, readHolding, writeReg, writeCoil, writeMulti,
  parseRead, parseAck, regs64ToUOhm, REG, COIL,
};
