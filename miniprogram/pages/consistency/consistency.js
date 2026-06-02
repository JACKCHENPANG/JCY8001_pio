const dev = require('../../utils/device.js');
const A = require('../../utils/analysis.js');

Page({
  data: {
    stage: 'setup',                 // setup | measure | done
    n: 4, spec: '', tol: 15, rmsThr: 5,
    idx: 1, prompt: '', busy: false, prog: '',
    rows: [],                       // 对比表
    median: 0, badText: '',
  },

  onN(e) { this.setData({ n: Math.max(1, parseInt(e.detail.value) || 1) }); },
  onSpec(e) { this.setData({ spec: e.detail.value }); },
  onTol(e) { this.setData({ tol: parseFloat(e.detail.value) || 15 }); },
  onRms(e) { this.setData({ rmsThr: parseFloat(e.detail.value) || 5 }); },

  onStart() {
    if (!getApp().globalData.ble) { wx.showToast({ title: '请先在首页连接', icon: 'none' }); return; }
    this.results = [];
    this.setData({ stage: 'measure', idx: 1, rows: [], badText: '' });
    this.showCell(1);
  },

  showCell(k) {
    this.setData({
      idx: k,
      prompt: '第 ' + k + '/' + this.data.n + ' 串:探针插入 B' + (k - 1) + ' (+) 和 B' + k + ' (−),注意正负',
    });
    this.drawPinout(this.data.n, k);
  },

  drawPinout(n, active) {
    const ctx = wx.createCanvasContext('pin', this);
    const npin = n + 1, W = 320, H = 90, m = 24;
    ctx.setFillStyle('#fff'); ctx.fillRect(0, 0, W, H);
    const step = (W - 2 * m) / Math.max(npin - 1, 1), cy = 40, r = Math.min(12, step * 0.32);
    for (let i = 0; i < npin; i++) {
      const cx = m + i * step;
      const isP = (i === active - 1), isM = (i === active);
      ctx.beginPath();
      ctx.setFillStyle(isP ? '#cf222e' : isM ? '#0071e3' : '#c8c8cc');
      ctx.arc(cx, cy, r, 0, 2 * Math.PI); ctx.fill();
      ctx.setFillStyle('#333'); ctx.setFontSize(10);
      ctx.fillText('B' + i, cx - 7, cy + r + 14);
      if (isP || isM) { ctx.setFillStyle('#fff'); ctx.setFontSize(14); ctx.fillText(isP ? '+' : '-', cx - 3, cy + 5); }
    }
    ctx.draw();
  },

  async onConfirm() {
    const ble = getApp().globalData.ble;
    if (!ble) { wx.showToast({ title: '未连接', icon: 'none' }); return; }
    if (this.data.busy) return;
    this.setData({ busy: true, prog: '自动选档…' });
    try {
      const r = await dev.runSweep(ble, { auto: true, fast: 1, avg: 1,
        onAutoRange: (ar) => this.setData({ prog: '自动档 ' + ar.R + 'Ω' + (ar.current_A ? ' I≈' + ar.current_A.toFixed(2) + 'A' : '') }),
        onPoint: (i) => this.setData({ prog: (i + 1) + '/20' }) });
      const a = A.analyze(r.hz, r.re, r.im);
      const e = A.ecmFit(r.hz, r.re, r.im);
      if (!e) { this.setData({ busy: false }); this.retry('拟合失败/点数不足'); return; }
      if (e.rms > this.data.rmsThr) { this.setData({ busy: false }); this.retry('RMS=' + e.rms.toFixed(1) + '% 超阈值'); return; }
      this.results.push({ k: this.data.idx, Rs: e.Rs_uOhm, Rct: e.Rct_uOhm, Cdl: e.Cdl, rms: e.rms });
      this.setData({ busy: false });
      const next = this.data.idx + 1;
      if (next <= this.data.n) this.showCell(next); else this.finish();
    } catch (err) {
      this.setData({ busy: false });
      this.retry(String(err.message || err));
    }
  },

  retry(msg) {
    wx.showModal({
      title: '第 ' + this.data.idx + ' 串数据不可用',
      content: msg + '\n请重新插好探针(检查正负/接触)重测。',
      showCancel: false, confirmText: '重测',
    });
  },

  finish() {
    const res = this.results;
    const rcts = res.map(r => r.Rct);
    const c = A.consistency(rcts, this.data.tol);
    const rows = res.map((r, i) => ({
      k: r.k, rs: r.Rs.toFixed(0), rct: r.Rct.toFixed(0), cdl: r.Cdl.toPrecision(2),
      dev: (c.dev[i] >= 0 ? '+' : '') + c.dev[i].toFixed(1),
      bad: Math.abs(c.dev[i]) > this.data.tol,
    }));
    const badText = c.bad.length
      ? '⚠ 超差串: ' + c.bad.map(b => '第' + (b.i + 1) + '串(' + (b.dev >= 0 ? '+' : '') + b.dev.toFixed(1) + '%)').join('、')
      : '✅ 全部在 ±' + this.data.tol + '% 内,一致性良好';
    this.setData({ stage: 'done', rows, median: c.median.toFixed(0), badText });
  },

  copyReport() {
    const d = this.data;
    let s = 'JCY8001 不拆包一致性报告\n规格:' + (d.spec || '-') + '  串数:' + d.n
      + '  Rct偏差:±' + d.tol + '%  中位:' + d.median + 'μΩ\n串,Rs_μΩ,Rct_μΩ,Cdl_F,偏差%,状态\n';
    d.rows.forEach(r => { s += [r.k, r.rs, r.rct, r.cdl, r.dev, r.bad ? '超差' : 'OK'].join(',') + '\n'; });
    wx.setClipboardData({ data: s, success: () => wx.showToast({ title: '报告已复制', icon: 'success' }) });
  },

  restart() { this.setData({ stage: 'setup' }); },
});
