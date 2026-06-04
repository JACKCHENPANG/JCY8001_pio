// JCY8001 电池阻抗测试数据后端 (零依赖: node:http + node:sqlite)
// 跑: pm2 start battery_api.js --node-args="--experimental-sqlite" --name battery-api
'use strict';
const http = require('node:http');
const fs = require('node:fs');
const crypto = require('node:crypto');
const { DatabaseSync } = require('node:sqlite');
const PORT = 3010;
const PHOTO_DIR = '/root/jcy-test-data/photos';
try { fs.mkdirSync(PHOTO_DIR, { recursive: true }); } catch {}
const TOKEN = process.env.JCY_API_TOKEN || 'jcy8001-prod-7f3a2c9b5e';   // 产线 token
const DB_PATH = '/root/jcy-test-data/battery_trace.db';

const db = new DatabaseSync(DB_PATH);
db.exec(`CREATE TABLE IF NOT EXISTS battery_impedance_tests(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_serial TEXT,
  user_phone TEXT,
  battery_code TEXT NOT NULL,
  spectrum_json TEXT,
  rs REAL, rct REAL, l_nh REAL,
  ecm_json TEXT, temp REAL, volt REAL,
  device_id TEXT, operator TEXT, raw_json TEXT,
  photo_path TEXT,
  measured_at TEXT,
  created_at TEXT DEFAULT (datetime('now','localtime'))
);`);
// 旧库补列(已存在则忽略)
try { db.exec(`ALTER TABLE battery_impedance_tests ADD COLUMN photo_path TEXT`); } catch {}
try { db.exec(`ALTER TABLE battery_impedance_tests ADD COLUMN device_serial TEXT`); } catch {}
try { db.exec(`CREATE INDEX IF NOT EXISTS idx_serial_time ON battery_impedance_tests(device_serial, measured_at)`); } catch {}
try { db.exec(`ALTER TABLE battery_impedance_tests ADD COLUMN chemistry TEXT`); } catch {}
try { db.exec(`ALTER TABLE battery_impedance_tests ADD COLUMN soc REAL`); } catch {}
try { db.exec(`ALTER TABLE battery_impedance_tests ADD COLUMN rct_norm REAL`); } catch {}
try { db.exec(`ALTER TABLE battery_impedance_tests ADD COLUMN norm_json TEXT`); } catch {}
db.exec(`CREATE INDEX IF NOT EXISTS idx_code_time ON battery_impedance_tests(battery_code, measured_at);`);
db.exec(`CREATE INDEX IF NOT EXISTS idx_user_time ON battery_impedance_tests(user_phone, measured_at);`);

const ins = db.prepare(`INSERT INTO battery_impedance_tests
 (device_serial,user_phone,battery_code,spectrum_json,rs,rct,l_nh,ecm_json,temp,volt,device_id,operator,raw_json,photo_path,measured_at,chemistry,soc,rct_norm,norm_json)
 VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`);

function savePhoto(dataUrl) {
  if (!dataUrl || typeof dataUrl !== 'string') return null;
  const m = dataUrl.match(/^data:image\/(\w+);base64,(.+)$/);
  if (!m) return null;
  const ext = m[1] === 'png' ? 'png' : 'jpg';
  const fn = Date.now() + '_' + crypto.randomBytes(4).toString('hex') + '.' + ext;
  fs.writeFileSync(PHOTO_DIR + '/' + fn, Buffer.from(m[2], 'base64'));
  return fn;
}

function send(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8',
    'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Headers': 'Authorization,Content-Type',
    'Access-Control-Allow-Methods': 'GET,POST,OPTIONS' });
  res.end(body);
}
function auth(req) {
  const h = req.headers['authorization'] || '';
  return h === 'Bearer ' + TOKEN;
}
// ===== 管理后台鉴权 (账号密码 -> 内存 session token) =====
const ADMIN_USER = process.env.JCY_ADMIN_USER || 'admin';
const ADMIN_PASS = process.env.JCY_ADMIN_PASS || 'jcy-admin-2026';
const sessions = new Map();   // token -> expiry(ms)
const SESSION_MS = 12 * 3600 * 1000;
function newSession() {
  const t = crypto.randomBytes(24).toString('hex');
  sessions.set(t, Date.now() + SESSION_MS);
  return t;
}
function adminAuth(req) {
  const t = req.headers['x-admin-token'] || '';
  const exp = sessions.get(t);
  if (!exp) return false;
  if (Date.now() > exp) { sessions.delete(t); return false; }
  return true;
}
function readBody(req) {
  return new Promise((resolve) => {
    let d = ''; req.on('data', c => { d += c; if (d.length > 5e6) req.destroy(); });
    req.on('end', () => { try { resolve(JSON.parse(d || '{}')); } catch { resolve(null); } });
  });
}

http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://x');
  if (req.method === 'OPTIONS') return send(res, 204, {});
  if (u.pathname === '/health') return send(res, 200, { ok: true, ts: new Date().toISOString() });

  // ===== 管理后台路由 (账号密码 session, 不走 Bearer) =====
  if (req.method === 'POST' && u.pathname === '/admin/login') {
    const b = await readBody(req);
    if (!b || b.user !== ADMIN_USER || b.pass !== ADMIN_PASS) return send(res, 401, { ok: false, err: '账号或密码错误' });
    return send(res, 200, { ok: true, token: newSession(), expires_in: SESSION_MS / 1000 });
  }
  if (u.pathname.startsWith('/admin/')) {
    if (!adminAuth(req)) return send(res, 401, { ok: false, err: '未登录或已过期' });
    if (req.method === 'GET' && u.pathname === '/admin/list') {
      const where = []; const args = [];
      const f = { serial: 'device_serial', code: 'battery_code', operator: 'operator' };
      for (const [k, col] of Object.entries(f)) { const v = u.searchParams.get(k); if (v) { where.push(col + ' LIKE ?'); args.push('%' + v + '%'); } }
      const from = u.searchParams.get('from'), to = u.searchParams.get('to');
      if (from) { where.push('measured_at>=?'); args.push(from); }
      if (to) { where.push('measured_at<=?'); args.push(to + 'T23:59:59'); }
      const lim = Math.min(parseInt(u.searchParams.get('limit') || '100'), 1000);
      const off = parseInt(u.searchParams.get('offset') || '0');
      const wsql = where.length ? ' WHERE ' + where.join(' AND ') : '';
      const total = db.prepare(`SELECT COUNT(*) n FROM battery_impedance_tests` + wsql).get(...args).n;
      const cols = `id,device_serial,operator,user_phone,battery_code,rs,rct,rct_norm,chemistry,soc,l_nh,temp,volt,photo_path,measured_at,created_at`;
      const rows = db.prepare(`SELECT ${cols} FROM battery_impedance_tests${wsql} ORDER BY id DESC LIMIT ? OFFSET ?`).all(...args, lim, off);
      return send(res, 200, { ok: true, total, rows });
    }
    if (req.method === 'GET' && u.pathname === '/admin/record') {
      const id = parseInt(u.searchParams.get('id'));
      return send(res, 200, { ok: true, row: db.prepare(`SELECT * FROM battery_impedance_tests WHERE id=?`).get(id) });
    }
    if (req.method === 'GET' && u.pathname === '/admin/stats') {
      const total = db.prepare(`SELECT COUNT(*) n FROM battery_impedance_tests`).get().n;
      const devices = db.prepare(`SELECT COUNT(DISTINCT device_serial) n FROM battery_impedance_tests WHERE device_serial IS NOT NULL`).get().n;
      const codes = db.prepare(`SELECT COUNT(DISTINCT battery_code) n FROM battery_impedance_tests`).get().n;
      const last7 = db.prepare(`SELECT COUNT(*) n FROM battery_impedance_tests WHERE measured_at>=?`).get(new Date(Date.now() - 7 * 864e5).toISOString()).n;
      const byDev = db.prepare(`SELECT device_serial s, COUNT(*) n, MAX(measured_at) last FROM battery_impedance_tests WHERE device_serial IS NOT NULL GROUP BY device_serial ORDER BY n DESC LIMIT 50`).all();
      return send(res, 200, { ok: true, total, devices, codes, last7, byDev });
    }
    if (req.method === 'GET' && u.pathname === '/admin/export') {
      const where = []; const args = [];
      const f = { serial: 'device_serial', code: 'battery_code', operator: 'operator' };
      for (const [k, col] of Object.entries(f)) { const v = u.searchParams.get(k); if (v) { where.push(col + ' LIKE ?'); args.push('%' + v + '%'); } }
      const from = u.searchParams.get('from'), to = u.searchParams.get('to');
      if (from) { where.push('measured_at>=?'); args.push(from); }
      if (to) { where.push('measured_at<=?'); args.push(to + 'T23:59:59'); }
      const wsql = where.length ? ' WHERE ' + where.join(' AND ') : '';
      const rows = db.prepare(`SELECT id,device_serial,operator,battery_code,rs,rct,rct_norm,chemistry,soc,l_nh,temp,volt,measured_at FROM battery_impedance_tests${wsql} ORDER BY id DESC LIMIT 50000`).all(...args);
      let csv = '﻿id,设备序列号,操作员,电池码,Rs_mOhm,Rct_mOhm,Rct归一_mOhm,化学,SOC,L_nH,温度,电压,测量时间\n';
      for (const r of rows) csv += [r.id, r.device_serial || '', r.operator || '', r.battery_code, mohm(r.rs), mohm(r.rct), mohm(r.rct_norm), r.chemistry || '', r.soc != null ? r.soc.toFixed(0) : '', r.l_nh ?? '', r.temp ?? '', r.volt ?? '', r.measured_at].join(',') + '\n';
      res.writeHead(200, { 'Content-Type': 'text/csv; charset=utf-8', 'Access-Control-Allow-Origin': '*' });
      return res.end(csv);
    }
    return send(res, 404, { ok: false, err: 'not found' });
  }

  if (!auth(req)) return send(res, 401, { ok: false, err: 'unauthorized' });

  if (req.method === 'POST' && u.pathname === '/save') {
    const b = await readBody(req);
    if (!b || !b.battery_code) return send(res, 400, { ok: false, err: 'battery_code required' });
    let photo = null;
    try { photo = savePhoto(b.photo); } catch (e) { photo = null; }
    const r = ins.run(
      str(b.device_serial), str(b.user_phone), String(b.battery_code), JSON.stringify(b.spectrum || null),
      num(b.rs), num(b.rct), num(b.l_nh), JSON.stringify(b.ecm || null),
      num(b.temp), num(b.volt), str(b.device_id), str(b.operator),
      JSON.stringify(b.raw || null), photo, str(b.measured_at) || new Date().toISOString(),
      str(b.chemistry), num(b.soc), num(b.rct_norm), JSON.stringify(b.norm || null));
    return send(res, 200, { ok: true, id: Number(r.lastInsertRowid), photo: photo });
  }
  if (req.method === 'GET' && u.pathname === '/list') {
    const code = u.searchParams.get('code');
    const phone = u.searchParams.get('phone');
    const lim = Math.min(parseInt(u.searchParams.get('limit') || '50'), 500);
    const cols = `id,device_serial,user_phone,battery_code,rs,rct,l_nh,temp,volt,photo_path,measured_at,created_at`;
    const where = []; const args = [];
    const serial = u.searchParams.get('serial');
    if (serial) { where.push('device_serial=?'); args.push(serial); }
    if (phone) { where.push('user_phone=?'); args.push(phone); }
    if (code) { where.push('battery_code=?'); args.push(code); }
    const sql = `SELECT ${cols} FROM battery_impedance_tests` +
      (where.length ? ' WHERE ' + where.join(' AND ') : '') + ' ORDER BY id DESC LIMIT ?';
    args.push(lim);
    return send(res, 200, { ok: true, rows: db.prepare(sql).all(...args) });
  }
  if (req.method === 'GET' && u.pathname === '/record') {
    const id = parseInt(u.searchParams.get('id'));
    const row = db.prepare(`SELECT * FROM battery_impedance_tests WHERE id=?`).get(id);
    return send(res, 200, { ok: true, row });
  }
  if (req.method === 'GET' && u.pathname === '/export') {
    const code = u.searchParams.get('code');
    const phone = u.searchParams.get('phone');
    const where = []; const args = [];
    if (phone) { where.push('user_phone=?'); args.push(phone); }
    if (code) { where.push('battery_code=?'); args.push(code); }
    const sql = `SELECT id,user_phone,battery_code,rs,rct,l_nh,temp,volt,measured_at FROM battery_impedance_tests` +
      (where.length ? ' WHERE ' + where.join(' AND ') : '') + ' ORDER BY id DESC LIMIT 5000';
    const rows = db.prepare(sql).all(...args);
    let csv = 'id,user_phone,battery_code,Rs_mOhm,Rct_mOhm,L_nH,temp,volt,measured_at\n';
    for (const r of rows) csv += [r.id, r.user_phone||'', r.battery_code, mohm(r.rs), mohm(r.rct), r.l_nh, r.temp, r.volt, r.measured_at].join(',') + '\n';
    res.writeHead(200, { 'Content-Type': 'text/csv; charset=utf-8', 'Access-Control-Allow-Origin': '*' });
    return res.end(csv);
  }
  if (req.method === 'GET' && u.pathname === '/count') {
    const c = db.prepare(`SELECT COUNT(*) n FROM battery_impedance_tests`).get();
    return send(res, 200, { ok: true, count: c.n });
  }
  send(res, 404, { ok: false, err: 'not found' });
}).listen(PORT, '127.0.0.1', () => console.log('battery-api on 127.0.0.1:' + PORT));

function num(v){ return (v===undefined||v===null||v==='')?null:Number(v); }
function str(v){ return (v===undefined||v===null)?null:String(v); }
function mohm(v){ return v==null?'':(v/1000).toFixed(4); }   // µΩ->mΩ for CSV
