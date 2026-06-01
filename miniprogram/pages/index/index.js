const { BleModbus } = require('../../utils/ble.js');
const dev = require('../../utils/device.js');
const A = require('../../utils/analysis.js');

Page({
  data: {
    status: '未连接', connected: false, busy: false,
    fw: '-', temp: '-', volt: '-',
    prog: '', rs: '-', rct: '-', lnh: '-',
    ecm: '', pts: [],
  },

  onConnect() {
    const app = getApp();
    if (this.data.connected) {           // 断开
      if (app.globalData.ble) app.globalData.ble.disconnect();
      app.globalData.ble = null; app.globalData.connected = false;
      this.setData({ connected: false, status: '未连接' });
      return;
    }
    const ble = new BleModbus();
    ble.onState = s => this.setData({ status: s });
    this.setData({ status: '扫描中…' });
    ble.scanConnect('JDY').then(async () => {
      app.globalData.ble = ble; app.globalData.connected = true;
      this.setData({ connected: true, status: '已连接' });
      const b = await dev.readBasic(ble);
      this.setData({
        fw: b.fw != null ? ('v' + b.fw.toString(16)) : '-',
        temp: b.temp != null ? b.temp.toFixed(1) + ' ℃' : '-',
        volt: b.volt != null ? b.volt.toFixed(4) + ' V' : '-',
      });
    }).catch(e => {
      this.setData({ status: '连接失败' });
      wx.showModal({ title: '蓝牙', content: String(e.message || e), showCancel: false });
    });
  },

  async onSweep() {
    const app = getApp();
    if (!app.globalData.ble) { wx.showToast({ title: '请先连接', icon: 'none' }); return; }
    this.setData({ busy: true, prog: '0/20', pts: [], rs: '-', rct: '-', lnh: '-', ecm: '测量中…' });
    const re = [], im = [], hz = [];
    try {
      const r = await dev.runSweep(app.globalData.ble, {
        samp: 2, fast: 1, avg: 1,
        onPoint: (i, f, x, y) => {
          hz.push(f); re.push(x); im.push(y);
          this.setData({ prog: (i + 1) + '/20' });
        },
      });
      const a = A.analyze(r.hz, r.re, r.im);
      const e = A.ecmFit(r.hz, r.re, r.im);
      this.setData({
        busy: false, prog: '完成',
        lnh: a.L_nH ? a.L_nH.toFixed(0) + ' nH (CV' + (a.cv * 100).toFixed(0) + '%)' : '-',
        rs: a.Rs ? a.Rs.toFixed(0) + ' μΩ' : '-',
        rct: a.Rct ? a.Rct.toFixed(0) + ' μΩ' : '-',
        ecm: e ? ('ECM  Rs=' + e.Rs_uOhm.toFixed(0) + '  Rct=' + e.Rct_uOhm.toFixed(0)
              + '  Cdl=' + e.Cdl.toPrecision(2) + 'F  n=' + e.n.toFixed(2)
              + '  RMS=' + e.rms.toFixed(1) + '%') : 'ECM 拟合失败',
      });
      this.drawNyquist(r.re, a.imc, a.artifact, a.circle);
    } catch (err) {
      this.setData({ busy: false, ecm: '出错: ' + String(err.message || err) });
    }
  },

  drawNyquist(re, imc, artifact, circle) {
    const ctx = wx.createCanvasContext('nyq', this);
    const W = 320, H = 240, m = 36;
    const isArt = i => artifact.indexOf(i) >= 0;
    const xs = re, ys = imc;
    let xmin = Math.min.apply(null, xs), xmax = Math.max.apply(null, xs);
    let ymin = Math.min.apply(null, ys), ymax = Math.max.apply(null, ys);
    if (xmax - xmin < 1) xmax = xmin + 1; if (ymax - ymin < 1) ymax = ymin + 1;
    const px = x => m + (x - xmin) / (xmax - xmin) * (W - 2 * m);
    const py = y => H - m - (y - ymin) / (ymax - ymin) * (H - 2 * m);
    ctx.setFillStyle('#fff'); ctx.fillRect(0, 0, W, H);
    // 轴
    ctx.setStrokeStyle('#ddd'); ctx.beginPath();
    ctx.moveTo(m, py(0)); ctx.lineTo(W - m, py(0)); ctx.stroke();
    // 拟合圆
    if (circle) {
      ctx.setStrokeStyle('#ff3b30'); ctx.beginPath();
      for (let t = 0; t <= 60; t++) {
        const th = Math.PI * t / 60, cx = circle.xc + circle.R * Math.cos(th), cy = circle.yc + circle.R * Math.sin(th);
        if (cy < ymin - 5) continue;
        const X = px(cx), Y = py(cy);
        if (t === 0) ctx.moveTo(X, Y); else ctx.lineTo(X, Y);
      }
      ctx.stroke();
    }
    // 数据点
    ctx.beginPath(); ctx.setStrokeStyle('#0071e3');
    let first = true;
    for (let i = 0; i < xs.length; i++) {
      if (isArt(i)) continue;
      const X = px(xs[i]), Y = py(ys[i]);
      if (first) { ctx.moveTo(X, Y); first = false; } else ctx.lineTo(X, Y);
    }
    ctx.stroke();
    for (let i = 0; i < xs.length; i++) {
      ctx.beginPath();
      ctx.setFillStyle(isArt(i) ? '#c8c8cc' : '#0071e3');
      ctx.arc(px(xs[i]), py(ys[i]), 3, 0, 2 * Math.PI); ctx.fill();
    }
    ctx.setFillStyle('#8e8e93'); ctx.setFontSize(10);
    ctx.fillText("Z' (μΩ)", W / 2 - 20, H - 6);
    ctx.draw();
  },

  goConsistency() { wx.navigateTo({ url: '/pages/consistency/consistency' }); },
});
