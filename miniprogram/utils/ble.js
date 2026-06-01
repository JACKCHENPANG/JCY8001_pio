// JDY-10 透传 BLE 传输 + Modbus 请求-响应 (微信小程序)。
// JDY-10/JDY系列/HC-42: Service FFE0, Characteristic FFE1 (write + notify 同一特征)。
'use strict';
const MB = require('./modbus.js');

const SVC = 'FFE0';
const CHR = 'FFE1';

function buf2hex(buf) {
  return Array.prototype.map.call(new Uint8Array(buf), b => b.toString(16).padStart(2, '0')).join('');
}
function ab(bytes) {                       // Uint8Array/array -> ArrayBuffer
  const u = (bytes instanceof Uint8Array) ? bytes : new Uint8Array(bytes);
  return u.buffer.slice(u.byteOffset, u.byteOffset + u.byteLength);
}

class BleModbus {
  constructor() {
    this.deviceId = null; this.svc = null; this.chr = null;
    this.rxbuf = [];                       // 累积收到的字节
    this._connected = false;
    this.onState = null;                   // 状态回调(可选)
  }
  _emit(s) { if (this.onState) this.onState(s); }

  // 扫描并连接 (按名字关键词, 默认匹配含 'JDY' 或全部含 FFE0 服务的)
  scanConnect(nameKeyword) {
    return new Promise((resolve, reject) => {
      wx.openBluetoothAdapter({
        success: () => {
          this._emit('扫描中…');
          wx.onBluetoothDeviceFound(res => {
            for (const d of res.devices) {
              const nm = (d.name || d.localName || '');
              if (nameKeyword ? nm.indexOf(nameKeyword) >= 0 : nm) {
                wx.stopBluetoothDevicesDiscovery();
                this._connect(d.deviceId).then(resolve).catch(reject);
                return;
              }
            }
          });
          wx.startBluetoothDevicesDiscovery({ allowDuplicatesKey: false });
        },
        fail: e => reject(new Error('蓝牙未开启: ' + JSON.stringify(e))),
      });
    });
  }

  _connect(deviceId) {
    this.deviceId = deviceId;
    return new Promise((resolve, reject) => {
      this._emit('连接中…');
      wx.createBLEConnection({
        deviceId, timeout: 8000,
        success: () => {
          wx.setBLEMTU({ deviceId, mtu: 247, complete: () => {
            wx.getBLEDeviceServices({ deviceId, success: r => {
              const s = r.services.find(x => x.uuid.toUpperCase().indexOf(SVC) >= 0) || r.services[0];
              this.svc = s.uuid;
              wx.getBLEDeviceCharacteristics({ deviceId, serviceId: this.svc, success: rc => {
                const ch = rc.characteristics.find(x => x.uuid.toUpperCase().indexOf(CHR) >= 0) || rc.characteristics[0];
                this.chr = ch.uuid;
                wx.notifyBLECharacteristicValueChange({
                  deviceId, serviceId: this.svc, characteristicId: this.chr, state: true,
                  success: () => {
                    wx.onBLECharacteristicValueChange(res => {
                      const u = new Uint8Array(res.value);
                      for (let i = 0; i < u.length; i++) this.rxbuf.push(u[i]);
                    });
                    this._connected = true; this._emit('已连接'); resolve(true);
                  },
                  fail: e => reject(new Error('notify失败:' + JSON.stringify(e))),
                });
              }, fail: e => reject(new Error('取特征失败:' + JSON.stringify(e))) });
            }, fail: e => reject(new Error('取服务失败:' + JSON.stringify(e))) });
          }});
        },
        fail: e => reject(new Error('连接失败:' + JSON.stringify(e))),
      });
    });
  }

  disconnect() {
    this._connected = false;
    if (this.deviceId) wx.closeBLEConnection({ deviceId: this.deviceId });
    wx.closeBluetoothAdapter();
  }

  // 写: BLE 单包 ≤20 字节, 分片发
  _write(bytes) {
    return new Promise((resolve, reject) => {
      const chunks = [];
      for (let i = 0; i < bytes.length; i += 20) chunks.push(bytes.slice(i, i + 20));
      const sendNext = idx => {
        if (idx >= chunks.length) { resolve(); return; }
        wx.writeBLECharacteristicValue({
          deviceId: this.deviceId, serviceId: this.svc, characteristicId: this.chr,
          value: ab(chunks[idx]),
          success: () => sendNext(idx + 1),
          fail: e => reject(new Error('写失败:' + JSON.stringify(e))),
        });
      };
      sendNext(0);
    });
  }

  // 发 Modbus 请求, 收完整帧后解析。kind: 'read'|'ack'
  request(reqBytes, kind, timeoutMs) {
    timeoutMs = timeoutMs || 1500;
    this.rxbuf = [];
    return this._write(reqBytes).then(() => new Promise((resolve) => {
      const t0 = Date.now();
      const tick = () => {
        const b = this.rxbuf;
        // 判断是否收齐
        let need = 5;
        if (b.length >= 2 && (b[1] & 0x80)) need = 5;                 // 异常帧
        else if (kind === 'read' && b.length >= 3) need = 3 + b[2] + 2;
        else if (kind === 'ack') need = (reqBytes.length >= 8 && reqBytes[1] === 0x10) ? 8 : reqBytes.length; // FC10回6字节
        if (b.length >= need) {
          const buf = b.slice(0, need);
          resolve(kind === 'read' ? MB.parseRead(buf) : { ok: MB.parseAck(buf) });
          return;
        }
        if (Date.now() - t0 > timeoutMs) { resolve({ ok: false, err: 'timeout' }); return; }
        setTimeout(tick, 20);
      };
      tick();
    }));
  }

  // 带重试的读保持寄存器 -> {ok, regs}
  readHolding(reg, count, retries) {
    retries = retries || 4;
    const attempt = (n) => this.request(MB.readHolding(reg, count), 'read').then(r => {
      if (r.ok || n <= 1) return r;
      return new Promise(res => setTimeout(res, 60)).then(() => attempt(n - 1));
    });
    return attempt(retries);
  }
  writeReg(reg, val) { return this.request(MB.writeReg(reg, val), 'ack'); }
  writeCoil(addr, on) { return this.request(MB.writeCoil(addr, on), 'ack'); }
  writeMulti(reg, vals) { return this.request(MB.writeMulti(reg, vals), 'ack'); }
}

module.exports = { BleModbus, SVC, CHR, buf2hex };
