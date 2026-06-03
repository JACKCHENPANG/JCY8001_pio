# JCY8001 Web 蓝牙版 (单文件)

`jcy8001_ble.html` — 安卓 Chrome 打开即用的 Web Bluetooth 上位机。
连 JDY-10(FFE0/FFE1) → Modbus → L补偿/Rs/Rct/ECM/Nyquist。
**不依赖微信、无需备案/认证/主体/付费。**

## 用法
- 安卓手机 **Chrome**(非微信内置浏览器) 打开本页 → 「连接蓝牙」→ 选 JDY-10 → 「开始扫频」
- Web Bluetooth 需安全上下文: HTTPS 链接 / localhost / file://(本地打开)
- iOS/微信内不支持 Web 蓝牙(安卓 Chrome 专用)

## 部署成链接(任选)
- GitHub Pages: 仓库 Settings→Pages 开启, 访问 https://<user>.github.io/JCY8001_pio/webapp/jcy8001_ble.html
- 任意静态托管 / 内网 nginx

逻辑与微信小程序 miniprogram/ 同源(modbus.js+analysis.js 内联), analysis 与 PC/固件逐位验过。
