# -*- coding: utf-8 -*-
"""
JCY8001 单通道电化学阻抗谱 (EIS) 上位机
=========================================
Python 3.8+  /  PyQt5  /  pyqtgraph  /  pyserial
单文件桌面程序，苹果风清爽配色，可 PyInstaller 打包为 exe。

通讯: Modbus RTU over 串口, 115200 8N1, 从机 0x01。
打包: pyinstaller -F -w -n JCY8001 main.py
依赖: pip install pyqt5 pyqtgraph pyserial
"""

import sys
import csv
import time
import math
import threading

from PyQt5 import QtCore, QtGui, QtWidgets
from PyQt5.QtCore import Qt, QThread, pyqtSignal, QTimer

import pyqtgraph as pg
import serial
import serial.tools.list_ports


# ============================================================================
#  寄存器地址表
# ============================================================================
class R:
    # --- 信息 ---
    CH_COUNT      = 0x3E00   # 通道数 (=1)
    FW_VERSION    = 0x3E02   # 固件版本 (0x0217 -> v2.17)
    BUILD_DATE    = 0x3E04   # 编译日期

    # --- 实时 ---
    TEMP          = 0x3300   # 温度 值/10 = ℃ (有符号)
    VOLTAGE       = 0x3340   # 电压 值/10000 = V
    RT_STATUS     = 0x3380   # 0x0001 测量中 / 0x0006 完成 / 0x0005 ADC错

    # --- 单点结果 ---
    SP_RE         = 0x3000   # 起 4 寄存器, 64位大端有符号, 值/100000 = μΩ
    SP_IM         = 0x3080   # 同上 (已是 -Z 虚部)
    SP_VZM        = 0x3200

    # --- 参数 (FC06 写 / FC03 读) ---
    P_SAMPLE      = 0x40C0   # 采样档 0=10Ω 1=5Ω 2=1Ω
    P_AVERAGE     = 0x4040   # 平均 1~64
    P_GAIN        = 0x4280   # 增益 1 / 4 / 16
    P_SPEED       = 0x4340   # 0 标准 / 1 快速
    P_MODE        = 0x4300   # 0 普通 / 1 低阻低频
    P_FREQ_CODE   = 0x4200   # 单点频率码 (M/E)

    # --- 校准 ---
    CAL_10R       = 0x40D0   # 10Ω 档实际 mΩ
    CAL_5R        = 0x40D1   # 5Ω  档实际 mΩ
    CAL_1R        = 0x40D2   # 1Ω  档实际 mΩ

    # --- 单点 ZM 启停线圈 ---
    COIL_SINGLE   = 0x0000

    # --- 固件自主扫频 ---
    SW_TABLE      = 0x4400   # 频点表起址 (FC10 一次写, 每点 1 寄存器 16位 M/E)
    SW_NPOINTS    = 0x43C0   # 点数 N (FC06)
    COIL_SWEEP    = 0x00C0   # 线圈 0xFF00 启 / 0 停
    SW_STATE      = 0x3E40   # 0 空闲 1 跑中 2 完成 3 中止
    SW_DONE_CNT   = 0x3E41   # 已完成点数
    SW_RE         = 0x3400   # 结果 RE = 0x3400 + idx*4
    SW_IM         = 0x3500   # 结果 IM = 0x3500 + idx*4
    SW_VZM        = 0x3600   # VZM    = 0x3600 + idx
    SW_FREQCODE   = 0x3640   # 实际频率码 = 0x3640 + idx


# 默认 20 点频点表 (M/E 码, 高频 -> 低频)
DEFAULT_CODES = [
    0x0B42, 0x09A2, 0x08C6, 0x087A, 0x0796, 0x082E, 0x073A, 0x0812,
    0x0716, 0x070E, 0x0806, 0x070A, 0x0902, 0x0706, 0x0802, 0x0702,
    0x0602, 0x0502, 0x0402, 0x0302,
]

# 换算常量: 主时钟 125 kHz, 24 位累加器
_FCLK = 125000.0
_FACTOR = _FCLK / (2 ** 24)   # ≈ 0.00745058 Hz / LSB


def code_to_hz(code: int) -> float:
    """M/E 频率码 -> Hz.  f = 尾数(低字节) * 2^指数(高字节) * 125000 / 2^24"""
    code &= 0xFFFF
    exponent = (code >> 8) & 0xFF   # M (高字节)
    mantissa = code & 0xFF          # E (低字节)
    return mantissa * (2 ** exponent) * _FACTOR


def to_int16(v: int) -> int:
    """16 位无符号 -> 有符号"""
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


# ============================================================================
#  Modbus RTU 通讯层
# ============================================================================
class ModbusError(Exception):
    pass


class Modbus:
    """线程安全的 Modbus RTU 主站。所有事务持锁串行执行。"""

    def __init__(self, addr=0x01, timeout=1.0, retries=4):
        # timeout = 单帧响应总等待上限(秒)。固件在 ZM 测量中单个忙窗口可 >0.3s,
        # 故响应需耐心等到 deadline, 不能一遇空读就放弃 (见 _read_exact)。
        self.ser = None
        self.addr = addr
        self.timeout = timeout
        self.retries = retries
        self.lock = threading.Lock()

    # ---- 串口 ----
    def open(self, port, baud=115200):
        self.close()
        self.ser = serial.Serial(
            port=port, baudrate=baud, bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE, stopbits=serial.STOPBITS_ONE,
            timeout=0.05, write_timeout=self.timeout,   # 单次read短超时, 总等待由 _read_exact 的 deadline 控制
        )

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    @property
    def is_open(self):
        return self.ser is not None and self.ser.is_open

    # ---- CRC16 (init 0xFFFF, poly 0xA001 右移, 帧尾 [低,高]) ----
    @staticmethod
    def crc16(data: bytes) -> bytes:
        crc = 0xFFFF
        for b in data:
            crc ^= b
            for _ in range(8):
                if crc & 1:
                    crc = (crc >> 1) ^ 0xA001
                else:
                    crc >>= 1
        return bytes([crc & 0xFF, (crc >> 8) & 0xFF])

    def _read_exact(self, n: int) -> bytes:
        # 持续累积读取直到凑齐 n 字节或 deadline 超时。不能一遇空读就 break——
        # 固件 ZM 测量中可能延迟若干百 ms 才回包, 早退会误判超时。
        buf = bytearray()
        deadline = time.time() + self.timeout
        while len(buf) < n and time.time() < deadline:
            chunk = self.ser.read(n - len(buf))
            if chunk:
                buf += chunk
        return bytes(buf)

    def _txn(self, pdu: bytes, resp_len: int) -> bytes:
        """发送一帧 (自动追加 CRC), 读回 resp_len 字节并校验; 带重试。"""
        if not self.is_open:
            raise ModbusError("串口未连接")
        frame = pdu + self.crc16(pdu)
        last = None
        for _ in range(self.retries):
            with self.lock:
                try:
                    self.ser.reset_input_buffer()
                    self.ser.reset_output_buffer()
                    self.ser.write(frame)
                    resp = self._read_exact(resp_len)
                except Exception as e:
                    last = ModbusError("串口IO错误: %s" % e)
                    continue
            if len(resp) < resp_len:
                last = ModbusError("响应超时/不足 (%d/%d)" % (len(resp), resp_len))
                time.sleep(0.02)
                continue
            if self.crc16(resp[:-2]) != resp[-2:]:
                last = ModbusError("CRC 校验失败")
                time.sleep(0.02)
                continue
            if resp[0] != pdu[0]:
                last = ModbusError("从机地址不符")
                time.sleep(0.02)
                continue
            if resp[1] & 0x80:
                code = resp[2] if len(resp) > 2 else 0
                last = ModbusError("Modbus 异常码 0x%02X" % code)
                time.sleep(0.02)
                continue
            return resp
        raise last or ModbusError("无响应")

    # ---- FC03 读保持寄存器 ----
    def read_holding(self, reg: int, count: int):
        pdu = bytes([self.addr, 0x03,
                     (reg >> 8) & 0xFF, reg & 0xFF,
                     (count >> 8) & 0xFF, count & 0xFF])
        resp = self._txn(pdu, 5 + 2 * count)
        bc = resp[2]
        data = resp[3:3 + bc]
        return [(data[i] << 8) | data[i + 1] for i in range(0, bc, 2)]

    # ---- FC04 读输入寄存器 ----
    def read_input(self, reg: int, count: int):
        pdu = bytes([self.addr, 0x04,
                     (reg >> 8) & 0xFF, reg & 0xFF,
                     (count >> 8) & 0xFF, count & 0xFF])
        resp = self._txn(pdu, 5 + 2 * count)
        bc = resp[2]
        data = resp[3:3 + bc]
        return [(data[i] << 8) | data[i + 1] for i in range(0, bc, 2)]

    # ---- FC06 写单寄存器 (等回显) ----
    def write_single(self, reg: int, val: int):
        val &= 0xFFFF
        pdu = bytes([self.addr, 0x06,
                     (reg >> 8) & 0xFF, reg & 0xFF,
                     (val >> 8) & 0xFF, val & 0xFF])
        resp = self._txn(pdu, 8)
        if resp[:6] != pdu:
            raise ModbusError("FC06 写回显不符 @0x%04X" % reg)

    # ---- FC10 写多寄存器 (等回显) ----
    def write_multiple(self, reg: int, values):
        count = len(values)
        bc = count * 2
        pdu = bytearray([self.addr, 0x10,
                         (reg >> 8) & 0xFF, reg & 0xFF,
                         (count >> 8) & 0xFF, count & 0xFF, bc])
        for v in values:
            v &= 0xFFFF
            pdu += bytes([(v >> 8) & 0xFF, v & 0xFF])
        resp = self._txn(bytes(pdu), 8)
        # 回显: addr,0x10,reg_hi,reg_lo,count_hi,count_lo,crc
        if resp[2:6] != bytes(pdu[2:6]):
            raise ModbusError("FC10 写回显不符 @0x%04X" % reg)

    # ---- FC05 写线圈 (0xFF00 ON / 0x0000 OFF, 等回显) ----
    def write_coil(self, coil: int, on: bool):
        val = 0xFF00 if on else 0x0000
        pdu = bytes([self.addr, 0x05,
                     (coil >> 8) & 0xFF, coil & 0xFF,
                     (val >> 8) & 0xFF, val & 0xFF])
        resp = self._txn(pdu, 8)
        if resp[:6] != pdu:
            raise ModbusError("FC05 线圈回显不符 @0x%04X" % coil)

    # ---- 读 64 位大端有符号 (4 寄存器) ----
    def read_int64(self, reg: int) -> int:
        regs = self.read_holding(reg, 4)
        raw = 0
        for r in regs:
            raw = (raw << 16) | (r & 0xFFFF)
        if raw >= (1 << 63):
            raw -= (1 << 64)
        return raw


# ============================================================================
#  扫频后台线程
# ============================================================================
class SweepWorker(QThread):
    point     = pyqtSignal(int, float, float, float, float)  # idx, hz, re_uΩ, im_uΩ, vzm
    progress  = pyqtSignal(int, int)                          # done, N
    status    = pyqtSignal(str)
    finished_ = pyqtSignal(int)                               # 2 完成 / 3 中止(用户/设备)
    failed    = pyqtSignal(str)

    def __init__(self, mb: Modbus, params, codes):
        super().__init__()
        self.mb = mb
        self.params = params          # [(reg, val), ...]
        self.codes = codes            # [m/e code, ...]
        self._stop = False

    def stop(self):
        self._stop = True

    def run(self):
        N = len(self.codes)
        try:
            # ① 写参数
            self.status.emit("写入参数…")
            for reg, val in self.params:
                self.mb.write_single(reg, val)
            # ② FC10 写频点表
            self.status.emit("写入频点表…")
            self.mb.write_multiple(R.SW_TABLE, self.codes)
            # ③ FC06 写点数 N
            self.mb.write_single(R.SW_NPOINTS, N)
            # ④ FC05 启动扫频
            self.mb.write_coil(R.COIL_SWEEP, True)
            self.status.emit("扫频中…")
            self.progress.emit(0, N)

            done = 0
            device_abort = False
            while not self._stop:
                completed = min(self.mb.read_holding(R.SW_DONE_CNT, 1)[0], N)
                # ⑤ 每新完成一点立即读取并画图
                while done < completed:
                    self._emit_point(done)
                    done += 1
                    self.progress.emit(done, N)

                state = self.mb.read_holding(R.SW_STATE, 1)[0]
                if state == 2:                       # ⑥ 完成
                    completed = min(self.mb.read_holding(R.SW_DONE_CNT, 1)[0], N)
                    while done < completed:
                        self._emit_point(done)
                        done += 1
                        self.progress.emit(done, N)
                    break
                if state == 3:                       # 设备中止
                    device_abort = True
                    break
                self.msleep(80)

            # ⑦ 停止扫频
            self._safe_stop_coil()

            if self._stop:
                self.status.emit("已停止")
                self.finished_.emit(3)
            elif device_abort:
                self.failed.emit("设备中止扫频 (状态=3)")
            else:
                self.status.emit("扫频完成")
                self.finished_.emit(2)

        except Exception as e:
            self._safe_stop_coil()
            self.failed.emit(str(e))

    def _emit_point(self, idx):
        re_raw = self.mb.read_int64(R.SW_RE + idx * 4)
        im_raw = self.mb.read_int64(R.SW_IM + idx * 4)
        vzm = self.mb.read_holding(R.SW_VZM + idx, 1)[0]
        code = self.mb.read_holding(R.SW_FREQCODE + idx, 1)[0]
        hz = code_to_hz(code) if code else code_to_hz(self.codes[idx])
        self.point.emit(idx, hz, re_raw / 100000.0, im_raw / 100000.0, float(vzm))

    def _safe_stop_coil(self):
        try:
            self.mb.write_coil(R.COIL_SWEEP, False)
        except Exception:
            pass


# ============================================================================
#  单点测量后台线程
# ============================================================================
class SingleWorker(QThread):
    done_   = pyqtSignal(float, float, float, float)   # hz, re_uΩ, im_uΩ, vzm
    failed  = pyqtSignal(str)
    status  = pyqtSignal(str)

    def __init__(self, mb: Modbus, params, code):
        super().__init__()
        self.mb = mb
        self.params = params
        self.code = code

    def run(self):
        try:
            self.status.emit("配置参数…")
            for reg, val in self.params:
                self.mb.write_single(reg, val)
            self.mb.write_single(R.P_FREQ_CODE, self.code)
            # 启动单点 ZM
            self.mb.write_coil(R.COIL_SINGLE, True)
            self.status.emit("测量中…")
            t0 = time.time()
            ok = False
            while time.time() - t0 < 30:
                st = self.mb.read_holding(R.RT_STATUS, 1)[0]
                if st == 0x0006:
                    ok = True
                    break
                if st == 0x0005:
                    raise ModbusError("ADC 错误 (状态=0x0005)")
                self.msleep(120)
            if not ok:
                raise ModbusError("单点测量超时")
            re = self.mb.read_int64(R.SP_RE) / 100000.0
            im = self.mb.read_int64(R.SP_IM) / 100000.0
            vzm = self.mb.read_holding(R.SP_VZM, 1)[0]
            self._safe_stop()
            self.done_.emit(code_to_hz(self.code), re, im, float(vzm))
        except Exception as e:
            self._safe_stop()
            self.failed.emit(str(e))

    def _safe_stop(self):
        try:
            self.mb.write_coil(R.COIL_SINGLE, False)
        except Exception:
            pass


# ============================================================================
#  苹果风样式
# ============================================================================
QSS = """
* {
    font-family: -apple-system, "SF Pro Text", "Helvetica Neue",
                 "PingFang SC", "Microsoft YaHei", sans-serif;
    font-size: 13px;
    color: #1d1d1f;
}
QWidget#central { background: #f5f5f7; }
QFrame#topbar {
    background: #ffffff;
    border: none;
    border-bottom: 1px solid #e3e3e6;
}
QGroupBox {
    background: #ffffff;
    border: 1px solid #e6e6ea;
    border-radius: 12px;
    margin-top: 16px;
    padding: 12px 12px 8px 12px;
    font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 14px;
    top: 2px;
    padding: 0 4px;
    color: #86868b;
    font-weight: 600;
}
QLabel#fieldLabel { color: #515154; }
QLabel#metricVal  { font-size: 15px; font-weight: 600; }
QLabel#metricCap  { color: #86868b; font-size: 11px; }

QPushButton {
    background: #ffffff;
    border: 1px solid #d2d2d7;
    border-radius: 8px;
    padding: 6px 14px;
}
QPushButton:hover   { background: #f2f2f4; }
QPushButton:pressed { background: #e8e8ec; }
QPushButton:disabled{ color: #b0b0b5; }

QPushButton#primary {
    background: #0071e3; color: #ffffff; border: none;
    border-radius: 10px; padding: 11px 22px;
    font-size: 15px; font-weight: 600;
}
QPushButton#primary:hover    { background: #0077ed; }
QPushButton#primary:pressed  { background: #006adb; }
QPushButton#primary:disabled { background: #aacdf3; }

QPushButton#danger {
    background: #ff3b30; color: #ffffff; border: none;
    border-radius: 10px; padding: 11px 22px;
    font-size: 15px; font-weight: 600;
}
QPushButton#danger:hover    { background: #ff5147; }
QPushButton#danger:disabled { background: #f4b6b2; }

QPushButton#connectBtn {
    background: #0071e3; color: #ffffff; border: none;
    border-radius: 8px; padding: 6px 16px; font-weight: 600;
}
QPushButton#connectBtn:hover { background: #0077ed; }
QPushButton#connectBtn[connected="true"] { background: #ff3b30; }

QComboBox, QSpinBox, QLineEdit {
    background: #ffffff; border: 1px solid #d2d2d7;
    border-radius: 8px; padding: 5px 8px; min-height: 22px;
}
QComboBox:focus, QSpinBox:focus, QLineEdit:focus { border: 1px solid #0071e3; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView {
    border: 1px solid #e3e3e6; border-radius: 8px;
    background: #ffffff; selection-background-color: #0071e3;
    selection-color: #ffffff; outline: none;
}

QProgressBar {
    border: none; border-radius: 7px; background: #e9e9eb;
    height: 14px; text-align: center; color: #515154; font-size: 11px;
}
QProgressBar::chunk { background: #0071e3; border-radius: 7px; }

QTableWidget {
    background: #ffffff; border: 1px solid #e6e6ea;
    border-radius: 10px; gridline-color: #f0f0f2;
}
QTableWidget::item { padding: 2px 6px; }
QHeaderView::section {
    background: #fafafa; border: none; border-bottom: 1px solid #e6e6ea;
    padding: 7px 6px; color: #86868b; font-weight: 600;
}

QTabBar::tab {
    background: transparent; padding: 8px 20px; margin-right: 4px;
    border-radius: 8px; color: #86868b; font-weight: 600;
}
QTabBar::tab:selected { background: #ffffff; color: #1d1d1f; }
QTabWidget::pane { border: none; top: 4px; }

QSplitter::handle { background: transparent; }
"""


def make_dot(color):
    """生成圆点 QLabel"""
    lab = QtWidgets.QLabel()
    lab.setFixedSize(11, 11)
    set_dot(lab, color)
    return lab


def set_dot(lab, color):
    lab.setStyleSheet(
        "background:%s; border-radius:5px; border:1px solid rgba(0,0,0,0.08);" % color
    )


COL_NYQ   = "#0071e3"
COL_BODE  = "#0071e3"
COL_PHASE = "#ff9500"
COL_ART   = "#8e8e93"   # 高频伪迹 (灰)
COL_FIT   = "#ff3b30"   # Rct 拟合圆 (红)
COL_RCT   = "#7d4cdb"   # Rct 跨距 (紫)


def analyze_eis(hz, re, im, hfmin=300.0, do_comp=True):
    """EIS 后处理: 拟合夹具串联电感 L → 减 jωL → 剔高频互感伪迹 → Rct 半圆拟合求 Rs/Rct。

    与 tools/eis_nyquist.py 同一套算法。入参 re/im 单位 μΩ, im 已是 -Z''。
    返回 dict: L_nH, cv, im_c(补偿后), artifact(伪迹下标), trusted(可信下标),
               Rs, Rct, circle=(xc,yc,R) 或 None。点不够时相应字段为 None。
    """
    import math as _m
    n = len(hz)
    out = dict(L_nH=None, cv=None, im_c=list(im), artifact=[],
               trusted=list(range(n)), Rs=None, Rct=None, circle=None)
    if n < 4 or not do_comp:
        return out
    # 1) 高频拟合 L: Z''=-im≈wL → 最小二乘过原点
    hf = [(h, -i) for h, i in zip(hz, im) if h >= hfmin]
    if len(hf) < 2:
        return out
    num = sum((zpp * 1e-6) * (2 * _m.pi * f) for f, zpp in hf)
    den = sum((2 * _m.pi * f) ** 2 for f, _ in hf)
    L = num / den
    Li = [(zpp * 1e-6) / (2 * _m.pi * f) for f, zpp in hf]
    mean = sum(Li) / len(Li)
    sd = (sum((x - mean) ** 2 for x in Li) / len(Li)) ** 0.5
    out["L_nH"] = L * 1e9
    out["cv"] = sd / mean if mean else 0.0
    # 2) 减 jωL
    im_c = [i + (2 * _m.pi * h) * L * 1e6 for h, i in zip(hz, im)]
    out["im_c"] = im_c
    # 3) 剔高频伪迹: 实部 Z'min 以上(更高频)的点 (列表按 HF->LF)
    i_min = min(range(n), key=lambda k: re[k])
    out["artifact"] = list(range(0, i_min))
    out["trusted"] = list(range(i_min, n))
    out["Rs"] = re[i_min]                      # 默认回退 = Z'min
    # 4) Rct 半圆拟合 (i_min..峰后第一个谷, 避开 Warburg)
    tr = out["trusted"]
    cand = [k for k in tr if hz[k] >= 5] or tr
    peak = max(cand, key=lambda k: im_c[k])
    we = n - 1
    for k in range(peak, n - 1):
        if im_c[k + 1] > im_c[k]:
            we = k; break
    arc = list(range(i_min, we + 1))
    if len(arc) >= 4:
        x = [re[k] for k in arc]; y = [im_c[k] for k in arc]
        # Kasa 圆拟合 (纯 python 解 3x3 正规方程)
        sx = sum(x); sy = sum(y); sxx = sum(a * a for a in x); syy = sum(b * b for b in y)
        sxy = sum(a * b for a, b in zip(x, y)); m = len(x)
        sxz = sum(a * (a * a + b * b) for a, b in zip(x, y))
        syz = sum(b * (a * a + b * b) for a, b in zip(x, y))
        sz = sum(a * a + b * b for a, b in zip(x, y))
        # 解 [[sxx sxy sx],[sxy syy sy],[sx sy m]]·[D E F]=-[sxz syz sz]
        A = [[sxx, sxy, sx], [sxy, syy, sy], [sx, sy, m]]
        rhs = [-sxz, -syz, -sz]
        sol = _solve3(A, rhs)
        if sol:
            D, E, F = sol; disc = D * D - 4 * F
            if disc > 0:
                r1 = (-D - disc ** 0.5) / 2; r2 = (-D + disc ** 0.5) / 2
                Rs, Rsum = min(r1, r2), max(r1, r2)
                out["Rs"] = Rs; out["Rct"] = Rsum - Rs
                xc, yc = -D / 2, -E / 2
                out["circle"] = (xc, yc, (xc * xc + yc * yc - F) ** 0.5)
    return out


def _solve3(A, b):
    """高斯消元解 3x3, 失败返回 None。"""
    M = [row[:] + [b[i]] for i, row in enumerate(A)]
    for c in range(3):
        p = max(range(c, 3), key=lambda r: abs(M[r][c]))
        if abs(M[p][c]) < 1e-12:
            return None
        M[c], M[p] = M[p], M[c]
        piv = M[c][c]
        for r in range(3):
            if r == c:
                continue
            f = M[r][c] / piv
            for k in range(c, 4):
                M[r][k] -= f * M[c][k]
    return [M[i][3] / M[i][i] for i in range(3)]


def ecm_fit(hz, re, im, i_min):
    """等效电路自动拟合 Randles+CPE+Warburg: Z=jωL+Rs+[CPE‖(Rct+Zw)]。
    在剔高频伪迹后的可信点上拟合。返回 dict 或 None(缺 scipy / 拟合失败)。
    re/im 单位 μΩ, im=-Z''。同 tools/eis_ecm.py。
    """
    try:
        import numpy as np
        from scipy.optimize import least_squares
    except Exception:
        return None
    try:
        hz = np.asarray(hz, float); re = np.asarray(re, float); im = np.asarray(im, float)
        w = 2 * np.pi * hz
        Z = (re - 1j * im) * 1e-6                  # Ω
        keep = np.arange(i_min, len(hz))
        if len(keep) < 6:
            return None
        wf, Zf = w[keep], Z[keep]

        def model(p, w):
            L, Rs, Rct, Q, n, sig = p
            jw = 1j * w
            Zp = 1.0 / (Q * (jw) ** n + 1.0 / (Rct + sig * (jw) ** -0.5))
            return 1j * w * L + Rs + Zp

        def resid(p):
            r = (model(p, wf) - Zf) / np.abs(Zf)
            return np.concatenate([r.real, r.imag])

        Rs0 = re[i_min] * 1e-6
        Rct0 = max((re[-1] - re[i_min]) * 1e-6 * 0.8, 1e-5)
        p0 = [2e-7, Rs0, Rct0, 10.0, 0.85, 5e-5]
        lb = [1e-9, 1e-6, 1e-6, 1e-3, 0.3, 0.0]
        ub = [1e-6, 5e-3, 5e-3, 1e5, 1.0, 1e-1]
        r = least_squares(resid, p0, bounds=(lb, ub), max_nfev=20000)
        L, Rs, Rct, Q, n, sig = r.x
        Cdl = (Q * (1 / Rs + 1 / Rct) ** (n - 1)) ** (1 / n)
        rms = float(np.sqrt(np.mean(np.abs((model(r.x, wf) - Zf) / np.abs(Zf)) ** 2)) * 100)
        wm = np.logspace(np.log10(wf.min()), np.log10(wf.max()), 240)
        Zm = model(r.x, wm)
        # 模型曲线也减掉 jωL, 与蓝色"已补偿"实测弧同坐标 (否则高频被电感拉下去, 看着不一致)
        my = (-Zm.imag + wm * L) * 1e6
        return dict(L=L, Rs=Rs, Rct=Rct, Q=Q, n=n, sig=sig, Cdl=Cdl, rms=rms,
                    fpk=1 / (2 * np.pi * Rct * Cdl),
                    mx=(Zm.real * 1e6).tolist(), my=my.tolist())
    except Exception:
        return None


# ============================================================================
#  自定义对数频率轴 (X 喂 log10(Hz), 标签显示真实 Hz)
# ============================================================================
class LogHzAxis(pg.AxisItem):
    def tickStrings(self, values, scale, spacing):
        out = []
        for v in values:
            f = 10 ** v
            if f >= 1000:
                out.append("%.1fk" % (f / 1000.0))
            elif f >= 10:
                out.append("%.0f" % f)
            elif f >= 1:
                out.append("%.1f" % f)
            else:
                out.append("%.2f" % f)
        return out


# ============================================================================
#  排针示意控件 (B0..BN 一排, 高亮当前测量的相邻两针, 标 +/-)
# ============================================================================
class PinoutWidget(QtWidgets.QWidget):
    def __init__(self):
        super().__init__()
        self.n_series = 0          # 串数 N -> 针 B0..BN (N+1 针)
        self.active = -1           # 当前电芯 k (1..N): 占用 B(k-1)+, B(k)-
        self.setMinimumHeight(110)

    def set_state(self, n_series, active):
        self.n_series = n_series; self.active = active; self.update()

    def paintEvent(self, ev):
        p = QtGui.QPainter(self)
        p.setRenderHint(QtGui.QPainter.Antialiasing)
        p.fillRect(self.rect(), QtGui.QColor("#ffffff"))
        npin = self.n_series + 1
        if npin < 2:
            return
        W = self.width(); H = self.height()
        m = 28
        step = (W - 2 * m) / max(npin - 1, 1)
        cy = H // 2 + 4
        r = min(14, int(step * 0.32))
        plus = self.active            # B(k-1) = +  (index active-1)
        minus = self.active           # B(k)   = -  (index active)
        for i in range(npin):
            cx = int(m + i * step)
            hi_p = (i == self.active - 1)
            hi_m = (i == self.active)
            if hi_p:
                col = QtGui.QColor("#cf222e")     # + 红
            elif hi_m:
                col = QtGui.QColor("#0071e3")     # - 蓝
            else:
                col = QtGui.QColor("#c8c8cc")
            p.setBrush(col); p.setPen(QtGui.QPen(QtGui.QColor("#888"), 1))
            p.drawEllipse(QtCore.QPoint(cx, cy), r, r)
            p.setPen(QtGui.QColor("#333"))
            p.drawText(QtCore.QRect(cx - 24, cy + r + 2, 48, 18),
                       Qt.AlignHCenter, "B%d" % i)
            if hi_p or hi_m:
                p.setPen(QtGui.QColor("#fff"))
                f = p.font(); f.setBold(True); f.setPointSize(12); p.setFont(f)
                p.drawText(QtCore.QRect(cx - r, cy - r, 2 * r, 2 * r),
                           Qt.AlignCenter, "+" if hi_p else "−")
                f.setBold(False); p.setFont(f)
        p.end()


# ============================================================================
#  主窗口
# ============================================================================
class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("JCY8001 · 电化学阻抗谱上位机  v1.0.0")
        self.resize(1280, 820)
        self.con_active = False    # 不拆包一致性测试进行中

        self.mb = Modbus()
        self.sweep_worker = None
        self.single_worker = None

        # 扫频数据缓存
        self.s_hz, self.s_re, self.s_im = [], [], []

        self._build_ui()

        # 温压状态轮询 (500ms)
        self.poll_timer = QTimer(self)
        self.poll_timer.setInterval(500)
        self.poll_timer.timeout.connect(self._poll)

        self.refresh_ports()
        self._set_connected_ui(False)

    # ---------------------------------------------------------------- UI
    def _build_ui(self):
        central = QtWidgets.QWidget()
        central.setObjectName("central")
        self.setCentralWidget(central)
        root = QtWidgets.QVBoxLayout(central)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        root.addWidget(self._build_topbar())

        body = QtWidgets.QWidget()
        body_l = QtWidgets.QVBoxLayout(body)
        body_l.setContentsMargins(14, 12, 14, 14)
        root.addWidget(body, 1)

        self.tabs = QtWidgets.QTabWidget()
        body_l.addWidget(self.tabs)
        self.tabs.addTab(self._build_sweep_tab(), "自主扫频")
        self.tabs.addTab(self._build_single_tab(), "单点测量")
        self.tabs.addTab(self._build_consistency_tab(), "不拆包测一致性")

        # 菜单: 工具 > 校准
        m = self.menuBar().addMenu("工具")
        act_cal = m.addAction("校准…")
        act_cal.triggered.connect(self._open_calibration)

        # 状态栏
        self.statusBar().showMessage("就绪")

    def _build_topbar(self):
        bar = QtWidgets.QFrame()
        bar.setObjectName("topbar")
        bar.setFixedHeight(60)
        h = QtWidgets.QHBoxLayout(bar)
        h.setContentsMargins(16, 8, 16, 8)
        h.setSpacing(10)

        title = QtWidgets.QLabel("JCY8001")
        title.setStyleSheet("font-size:17px; font-weight:700; letter-spacing:0.5px;")
        h.addWidget(title)
        h.addSpacing(8)

        self.cmb_port = QtWidgets.QComboBox()
        self.cmb_port.setMinimumWidth(150)
        h.addWidget(self.cmb_port)

        self.btn_refresh = QtWidgets.QPushButton("刷新")
        self.btn_refresh.clicked.connect(self.refresh_ports)
        h.addWidget(self.btn_refresh)

        self.cmb_baud = QtWidgets.QComboBox()
        self.cmb_baud.addItems(["115200", "57600", "38400", "19200", "9600"])
        h.addWidget(self.cmb_baud)

        self.btn_connect = QtWidgets.QPushButton("连接")
        self.btn_connect.setObjectName("connectBtn")
        self.btn_connect.clicked.connect(self.toggle_connect)
        h.addWidget(self.btn_connect)

        # 状态点
        self.dot = make_dot("#c7c7cc")
        h.addSpacing(10)
        h.addWidget(self.dot)
        self.lbl_conn = QtWidgets.QLabel("未连接")
        self.lbl_conn.setStyleSheet("color:#86868b;")
        h.addWidget(self.lbl_conn)

        h.addStretch(1)

        # 指标
        self.lbl_fw     = self._metric("固件", "—")
        self.lbl_temp   = self._metric("温度", "—")
        self.lbl_volt   = self._metric("电压", "—")
        self.lbl_devst  = self._metric("设备状态", "—")
        for w in (self.lbl_fw, self.lbl_temp, self.lbl_volt, self.lbl_devst):
            h.addWidget(w)
            h.addSpacing(6)

        return bar

    def _metric(self, cap, val):
        box = QtWidgets.QWidget()
        v = QtWidgets.QVBoxLayout(box)
        v.setContentsMargins(8, 0, 8, 0)
        v.setSpacing(0)
        lc = QtWidgets.QLabel(cap)
        lc.setObjectName("metricCap")
        lv = QtWidgets.QLabel(val)
        lv.setObjectName("metricVal")
        v.addWidget(lc)
        v.addWidget(lv)
        box._val = lv
        return box

    # --------------------------------------------------- 扫频页签
    def _build_sweep_tab(self):
        w = QtWidgets.QWidget()
        lay = QtWidgets.QHBoxLayout(w)
        lay.setContentsMargins(2, 6, 2, 2)
        lay.setSpacing(12)

        # ---- 左侧参数面板 ----
        left = QtWidgets.QWidget()
        left.setFixedWidth(290)
        ll = QtWidgets.QVBoxLayout(left)
        ll.setContentsMargins(0, 0, 0, 0)
        ll.setSpacing(12)

        gp = QtWidgets.QGroupBox("测量参数")
        form = QtWidgets.QFormLayout(gp)
        form.setLabelAlignment(Qt.AlignLeft)
        form.setSpacing(10)

        self.cmb_sample = QtWidgets.QComboBox()
        self.cmb_sample.addItems(["10 Ω", "5 Ω", "1 Ω"])     # index = 寄存器值
        self.cmb_gain = QtWidgets.QComboBox()
        self.cmb_gain.addItems(["1", "4", "16"])              # 值 1/4/16
        self.spn_avg = QtWidgets.QSpinBox()
        self.spn_avg.setRange(1, 64)
        self.spn_avg.setValue(4)
        self.cmb_speed = QtWidgets.QComboBox()
        self.cmb_speed.addItems(["标准", "快速"])             # 0 / 1
        self.cmb_mode = QtWidgets.QComboBox()
        self.cmb_mode.addItems(["普通", "低阻低频"])          # 0 / 1
        self.spn_npts = QtWidgets.QSpinBox()
        self.spn_npts.setRange(2, 20)
        self.spn_npts.setValue(20)
        self.spn_npts.valueChanged.connect(self._update_freq_hint)

        for lbl, wdg in [
            ("采样电阻档", self.cmb_sample),
            ("增益", self.cmb_gain),
            ("平均次数", self.spn_avg),
            ("速度", self.cmb_speed),
            ("模式", self.cmb_mode),
            ("点数 N", self.spn_npts),
        ]:
            la = QtWidgets.QLabel(lbl)
            la.setObjectName("fieldLabel")
            form.addRow(la, wdg)

        self.lbl_freq_hint = QtWidgets.QLabel()
        self.lbl_freq_hint.setObjectName("metricCap")
        self.lbl_freq_hint.setWordWrap(True)
        form.addRow(self.lbl_freq_hint)
        ll.addWidget(gp)

        # 控制
        gc = QtWidgets.QGroupBox("扫频控制")
        gcl = QtWidgets.QVBoxLayout(gc)
        gcl.setSpacing(10)
        self.btn_start = QtWidgets.QPushButton("开始扫频")
        self.btn_start.setObjectName("primary")
        self.btn_start.clicked.connect(self.start_sweep)
        self.btn_stop = QtWidgets.QPushButton("停止")
        self.btn_stop.setObjectName("danger")
        self.btn_stop.clicked.connect(self.stop_sweep)
        self.btn_stop.setEnabled(False)
        self.prog = QtWidgets.QProgressBar()
        self.prog.setValue(0)
        self.lbl_progtxt = QtWidgets.QLabel("0 / 0")
        self.lbl_progtxt.setObjectName("metricCap")
        self.lbl_progtxt.setAlignment(Qt.AlignCenter)
        gcl.addWidget(self.btn_start)
        gcl.addWidget(self.btn_stop)
        gcl.addWidget(self.prog)
        gcl.addWidget(self.lbl_progtxt)
        ll.addWidget(gc)

        # 导出
        self.btn_export = QtWidgets.QPushButton("导出 CSV")
        self.btn_export.clicked.connect(self.export_csv)
        ll.addWidget(self.btn_export)
        ll.addStretch(1)
        lay.addWidget(left)

        # ---- 右侧: 图 + 表 ----
        right = QtWidgets.QSplitter(Qt.Vertical)

        plots = QtWidgets.QSplitter(Qt.Horizontal)
        plots.addWidget(self._build_nyquist())
        plots.addWidget(self._build_bode())
        plots.setSizes([520, 520])
        right.addWidget(plots)

        # 数据表
        tbl_box = QtWidgets.QWidget()
        tbv = QtWidgets.QVBoxLayout(tbl_box)
        tbv.setContentsMargins(0, 0, 0, 0)
        self.table = QtWidgets.QTableWidget(0, 6)
        self.table.setHorizontalHeaderLabels(
            ["#", "频率 (Hz)", "RE (μΩ)", "IM (μΩ)", "|Z| (μΩ)", "相位 (°)"])
        self.table.verticalHeader().setVisible(False)
        self.table.horizontalHeader().setSectionResizeMode(
            QtWidgets.QHeaderView.Stretch)
        self.table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        tbv.addWidget(self.table)
        right.addWidget(tbl_box)

        right.setSizes([500, 230])
        lay.addWidget(right, 1)

        self._update_freq_hint()
        return w

    def _build_nyquist(self):
        gb = QtWidgets.QGroupBox("Nyquist")
        v = QtWidgets.QVBoxLayout(gb)
        v.setContentsMargins(8, 8, 8, 8)

        # 顶部: 电感补偿开关 + 结果
        top = QtWidgets.QHBoxLayout()
        self.chk_comp = QtWidgets.QCheckBox("电感补偿 + 拟合 Rs/Rct")
        self.chk_comp.setChecked(True)
        self.chk_comp.stateChanged.connect(self._redraw)
        self.lbl_fit = QtWidgets.QLabel("Rs/Rct: —")
        self.lbl_fit.setStyleSheet("font-weight:600;")
        top.addWidget(self.chk_comp)
        top.addStretch(1)
        top.addWidget(self.lbl_fit)
        v.addLayout(top)

        # 等效电路 (ECM) 全参数行
        self.lbl_ecm = QtWidgets.QLabel("ECM: —")
        self.lbl_ecm.setStyleSheet("color:#cf222e;")
        v.addWidget(self.lbl_ecm)

        self.p_nyq = pg.PlotWidget()
        self.p_nyq.setBackground("w")
        self.p_nyq.showGrid(x=True, y=True, alpha=0.25)
        self.p_nyq.setLabel("bottom", "RE", units="μΩ")
        self.p_nyq.setLabel("left", "IM (-Z'')", units="μΩ")
        self.p_nyq.setAspectLocked(True)
        # ECM 模型曲线 (红实线) — 最底层
        self.nyq_ecm = self.p_nyq.plot(
            [], [], pen=pg.mkPen(COL_FIT, width=1.6))
        # 拟合圆 (红虚线)
        self.nyq_fit = self.p_nyq.plot(
            [], [], pen=pg.mkPen(COL_FIT, width=1.2, style=Qt.DashLine))
        # 主曲线 (可信弧)
        self.nyq_curve = self.p_nyq.plot(
            [], [], pen=pg.mkPen(COL_NYQ, width=2),
            symbol="o", symbolSize=8,
            symbolBrush=COL_NYQ, symbolPen=pg.mkPen("w", width=1.2))
        # 高频伪迹 (灰 x)
        self.nyq_art = pg.ScatterPlotItem(
            size=10, symbol="x", pen=pg.mkPen(COL_ART, width=1.6), brush=None)
        self.p_nyq.addItem(self.nyq_art)
        # Rs / Rs+Rct 标记 (红)
        self.nyq_marks = pg.ScatterPlotItem(
            size=13, symbol="star", brush=pg.mkBrush(COL_FIT), pen=pg.mkPen("w", width=1))
        self.p_nyq.addItem(self.nyq_marks)
        v.addWidget(self.p_nyq)
        return gb

    def _build_bode(self):
        gb = QtWidgets.QGroupBox("Bode")
        v = QtWidgets.QVBoxLayout(gb)
        v.setContentsMargins(8, 8, 8, 8)
        self.p_bode = pg.PlotWidget(axisItems={"bottom": LogHzAxis(orientation="bottom")})
        self.p_bode.setBackground("w")
        self.p_bode.showGrid(x=True, y=True, alpha=0.25)
        self.p_bode.setLabel("bottom", "频率 (Hz)")
        self.p_bode.setLabel("left", "|Z| (μΩ)", color=COL_BODE)
        self.p_bode.getAxis("left").setTextPen(COL_BODE)

        self.z_curve = self.p_bode.plot(
            [], [], pen=pg.mkPen(COL_BODE, width=2),
            symbol="o", symbolSize=7,
            symbolBrush=COL_BODE, symbolPen=pg.mkPen("w", width=1))

        # 右轴 (相位) 第二 ViewBox
        self.phase_vb = pg.ViewBox()
        self.p_bode.scene().addItem(self.phase_vb)
        self.p_bode.showAxis("right")
        self.p_bode.getAxis("right").linkToView(self.phase_vb)
        self.p_bode.getAxis("right").setLabel("相位 (°)", color=COL_PHASE)
        self.p_bode.getAxis("right").setTextPen(COL_PHASE)
        self.phase_vb.setXLink(self.p_bode.getViewBox())

        self.phase_curve = pg.PlotCurveItem(pen=pg.mkPen(COL_PHASE, width=2))
        self.phase_scatter = pg.ScatterPlotItem(
            size=7, brush=pg.mkBrush(COL_PHASE), pen=pg.mkPen("w", width=1))
        self.phase_vb.addItem(self.phase_curve)
        self.phase_vb.addItem(self.phase_scatter)

        self.p_bode.getViewBox().sigResized.connect(self._sync_phase_vb)
        QTimer.singleShot(0, self._sync_phase_vb)
        v.addWidget(self.p_bode)
        return gb

    def _sync_phase_vb(self):
        self.phase_vb.setGeometry(self.p_bode.getViewBox().sceneBoundingRect())
        self.phase_vb.linkedViewChanged(self.p_bode.getViewBox(), self.phase_vb.XAxis)

    # --------------------------------------------------- 单点页签
    def _build_single_tab(self):
        w = QtWidgets.QWidget()
        lay = QtWidgets.QHBoxLayout(w)
        lay.setContentsMargins(2, 6, 2, 2)
        lay.setSpacing(12)

        left = QtWidgets.QWidget()
        left.setFixedWidth(290)
        ll = QtWidgets.QVBoxLayout(left)
        ll.setContentsMargins(0, 0, 0, 0)
        ll.setSpacing(12)

        gp = QtWidgets.QGroupBox("单点设置")
        form = QtWidgets.QFormLayout(gp)
        form.setSpacing(10)
        self.cmb_sp_freq = QtWidgets.QComboBox()
        for c in DEFAULT_CODES:
            self.cmb_sp_freq.addItem("%.3f Hz  (0x%04X)" % (code_to_hz(c), c), c)
        la = QtWidgets.QLabel("频率点")
        la.setObjectName("fieldLabel")
        form.addRow(la, self.cmb_sp_freq)
        ll.addWidget(gp)

        self.btn_sp_start = QtWidgets.QPushButton("开始单点测量")
        self.btn_sp_start.setObjectName("primary")
        self.btn_sp_start.clicked.connect(self.start_single)
        ll.addWidget(self.btn_sp_start)
        ll.addStretch(1)
        lay.addWidget(left)

        # 结果卡片
        res = QtWidgets.QGroupBox("测量结果")
        rl = QtWidgets.QGridLayout(res)
        rl.setSpacing(16)
        rl.setContentsMargins(20, 24, 20, 20)
        self.sp_cards = {}
        defs = [("频率", "Hz"), ("RE", "μΩ"), ("IM (-Z'')", "μΩ"),
                ("|Z|", "μΩ"), ("相位", "°"), ("VZM", "")]
        for i, (name, unit) in enumerate(defs):
            card = QtWidgets.QFrame()
            card.setStyleSheet(
                "background:#f5f5f7; border-radius:12px;")
            cv = QtWidgets.QVBoxLayout(card)
            cv.setContentsMargins(18, 16, 18, 16)
            cap = QtWidgets.QLabel("%s%s" % (name, ("  (%s)" % unit if unit else "")))
            cap.setObjectName("metricCap")
            val = QtWidgets.QLabel("—")
            val.setStyleSheet("font-size:26px; font-weight:700;")
            cv.addWidget(cap)
            cv.addWidget(val)
            rl.addWidget(card, i // 2, i % 2)
            self.sp_cards[name] = val
        lay.addWidget(res, 1)
        return w

    # ---------------------------------------------------------------- 串口
    def refresh_ports(self):
        cur = self.cmb_port.currentData()
        self.cmb_port.clear()
        for p in serial.tools.list_ports.comports():
            self.cmb_port.addItem("%s  %s" % (p.device, p.description or ""), p.device)
        if self.cmb_port.count() == 0:
            self.cmb_port.addItem("未发现串口", None)
        # 还原选择
        idx = self.cmb_port.findData(cur)
        if idx >= 0:
            self.cmb_port.setCurrentIndex(idx)

    def toggle_connect(self):
        if self.mb.is_open:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self.cmb_port.currentData()
        if not port:
            self._warn("请先选择串口")
            return
        baud = int(self.cmb_baud.currentText())
        try:
            self.mb.open(port, baud)
        except Exception as e:
            self._warn("打开串口失败:\n%s" % e)
            return
        # 读设备信息
        try:
            fw = self.mb.read_holding(R.FW_VERSION, 1)[0]
            self.lbl_fw._val.setText("v%X.%02X" % ((fw >> 8) & 0xFF, fw & 0xFF))
        except Exception as e:
            self.statusBar().showMessage("已连接, 但读取固件版本失败: %s" % e)
            self.lbl_fw._val.setText("?")
        self._set_connected_ui(True)
        self.poll_timer.start()
        self.statusBar().showMessage("已连接 %s @ %d" % (port, baud))

    def _disconnect(self):
        self.stop_sweep()
        self.poll_timer.stop()
        self.mb.close()
        self._set_connected_ui(False)
        for m in (self.lbl_fw, self.lbl_temp, self.lbl_volt, self.lbl_devst):
            m._val.setText("—")
        self.statusBar().showMessage("已断开")

    def _set_connected_ui(self, on):
        self.btn_connect.setText("断开" if on else "连接")
        self.btn_connect.setProperty("connected", "true" if on else "false")
        self.btn_connect.style().unpolish(self.btn_connect)
        self.btn_connect.style().polish(self.btn_connect)
        set_dot(self.dot, "#34c759" if on else "#c7c7cc")
        self.lbl_conn.setText("已连接" if on else "未连接")
        self.cmb_port.setEnabled(not on)
        self.cmb_baud.setEnabled(not on)
        self.btn_refresh.setEnabled(not on)
        self.btn_start.setEnabled(on)
        self.btn_sp_start.setEnabled(on)

    # ---------------------------------------------------------------- 轮询
    def _poll(self):
        if not self.mb.is_open:
            return
        try:
            t = to_int16(self.mb.read_holding(R.TEMP, 1)[0]) / 10.0
            self.lbl_temp._val.setText("%.1f ℃" % t)
        except Exception:
            pass
        try:
            v = self.mb.read_holding(R.VOLTAGE, 1)[0] / 10000.0
            self.lbl_volt._val.setText("%.4f V" % v)
        except Exception:
            pass
        try:
            st = self.mb.read_holding(R.RT_STATUS, 1)[0]
            self.lbl_devst._val.setText(self._status_text(st))
        except Exception:
            pass

    @staticmethod
    def _status_text(st):
        return {0x0001: "测量中", 0x0006: "完成",
                0x0005: "ADC错误"}.get(st, "0x%04X" % st)

    # ---------------------------------------------------------------- 参数
    def _build_params(self):
        gain_val = int(self.cmb_gain.currentText())
        return [
            (R.P_SAMPLE,  self.cmb_sample.currentIndex()),
            (R.P_GAIN,    gain_val),
            (R.P_AVERAGE, self.spn_avg.value()),
            (R.P_SPEED,   self.cmb_speed.currentIndex()),
            (R.P_MODE,    self.cmb_mode.currentIndex()),
        ]

    def _update_freq_hint(self):
        n = self.spn_npts.value()
        codes = DEFAULT_CODES[:n]
        hi = code_to_hz(codes[0])
        lo = code_to_hz(codes[-1])
        self.lbl_freq_hint.setText(
            "频段: %.3f Hz → %.3f Hz   (前 %d 点)" % (hi, lo, n))

    # ---------------------------------------------------------------- 扫频
    def start_sweep(self):
        if not self.mb.is_open:
            self._warn("请先连接设备")
            return
        if self.sweep_worker and self.sweep_worker.isRunning():
            return
        n = self.spn_npts.value()
        codes = DEFAULT_CODES[:n]

        # 清空
        self.s_hz, self.s_re, self.s_im = [], [], []
        self._redraw()
        self.table.setRowCount(0)
        self.prog.setMaximum(n)
        self.prog.setValue(0)
        self.lbl_progtxt.setText("0 / %d" % n)

        # 扫频时暂停温压轮询 (避免抢串口)
        self.poll_timer.stop()

        self.sweep_worker = SweepWorker(self.mb, self._build_params(), codes)
        self.sweep_worker.point.connect(self._on_point)
        self.sweep_worker.progress.connect(self._on_progress)
        self.sweep_worker.status.connect(self.statusBar().showMessage)
        self.sweep_worker.finished_.connect(self._on_sweep_finished)
        self.sweep_worker.failed.connect(self._on_sweep_failed)
        self.sweep_worker.start()

        self.btn_start.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self._lock_params(True)

    def stop_sweep(self):
        if self.sweep_worker and self.sweep_worker.isRunning():
            self.btn_stop.setEnabled(False)
            self.statusBar().showMessage("正在停止…")
            self.sweep_worker.stop()

    def _on_progress(self, done, n):
        self.prog.setMaximum(n)
        self.prog.setValue(done)
        self.lbl_progtxt.setText("%d / %d" % (done, n))

    def _on_point(self, idx, hz, re, im, vzm):
        self.s_hz.append(hz)
        self.s_re.append(re)
        self.s_im.append(im)
        mag = math.hypot(re, im)
        phase = math.degrees(math.atan2(im, re))

        # 数据表
        row = self.table.rowCount()
        self.table.insertRow(row)
        vals = ["%d" % (idx + 1), "%.3f" % hz, "%.2f" % re,
                "%.2f" % im, "%.2f" % mag, "%.2f" % phase]
        for c, s in enumerate(vals):
            it = QtWidgets.QTableWidgetItem(s)
            it.setTextAlignment(Qt.AlignCenter)
            self.table.setItem(row, c, it)
        self.table.scrollToBottom()
        self._redraw()

    def _redraw(self):
        # ── Nyquist + 电感补偿/拟合 ──
        comp_on = self.chk_comp.isChecked() if hasattr(self, "chk_comp") else False
        if comp_on and len(self.s_hz) >= 4:
            a = analyze_eis(self.s_hz, self.s_re, self.s_im)
            im_c = a["im_c"]; tr = a["trusted"]; art = a["artifact"]
            self.nyq_curve.setData([self.s_re[k] for k in tr], [im_c[k] for k in tr])
            self.nyq_art.setData([self.s_re[k] for k in art], [im_c[k] for k in art])
            marks = []
            if a["circle"] is not None:
                import math as _m
                xc, yc, R = a["circle"]
                xs_c, ys_c = [], []
                for j in range(201):
                    th = _m.pi * j / 200.0
                    yy = yc + R * _m.sin(th)
                    if yy >= -1:
                        xs_c.append(xc + R * _m.cos(th)); ys_c.append(yy)
                self.nyq_fit.setData(xs_c, ys_c)
                marks = [(a["Rs"], 0.0), (a["Rs"] + a["Rct"], 0.0)]
            else:
                self.nyq_fit.setData([], [])
                if a["Rs"] is not None:
                    marks = [(a["Rs"], 0.0)]
            self.nyq_marks.setData([p[0] for p in marks], [p[1] for p in marks])
            txt = "Rs/Rct: —"
            if a["L_nH"] is not None:
                cvp = ("CV%.0f%%" % (a["cv"] * 100)) if a["cv"] is not None else ""
                rct = ("  Rct=%.0f μΩ" % a["Rct"]) if a["Rct"] is not None else ""
                txt = "L=%.0f nH(%s)  Rs=%.0f μΩ%s" % (a["L_nH"], cvp, a["Rs"], rct)
            self.lbl_fit.setText(txt)
            # ── 等效电路 (ECM) 自动拟合 + 模型叠加 ──
            e = ecm_fit(self.s_hz, self.s_re, self.s_im, len(art)) if len(self.s_hz) >= 8 else None
            if e is not None:
                self.nyq_ecm.setData(e["mx"], e["my"])
                self.lbl_ecm.setText(
                    "ECM  L=%.0fnH  Rs=%.0fμΩ  Rct=%.0fμΩ  Cdl=%.2gF  CPE-n=%.2f  σ=%.2g  (RMS%.1f%%)"
                    % (e["L"] * 1e9, e["Rs"] * 1e6, e["Rct"] * 1e6, e["Cdl"], e["n"], e["sig"], e["rms"]))
            else:
                self.nyq_ecm.setData([], [])
                self.lbl_ecm.setText("ECM: —" + ("  (需 scipy)" if len(self.s_hz) >= 8 else ""))
        else:
            self.nyq_curve.setData(self.s_re, self.s_im)
            self.nyq_fit.setData([], [])
            self.nyq_ecm.setData([], [])
            self.nyq_art.setData([], [])
            self.nyq_marks.setData([], [])
            if hasattr(self, "lbl_fit"):
                self.lbl_fit.setText("Rs/Rct: —  (补偿关闭)" if not comp_on else "Rs/Rct: 点数不足")
                self.lbl_ecm.setText("ECM: —")
        # Bode (x = log10 Hz)
        xs, zs, ph = [], [], []
        for hz, re, im in zip(self.s_hz, self.s_re, self.s_im):
            if hz <= 0:
                continue
            xs.append(math.log10(hz))
            zs.append(math.hypot(re, im))
            ph.append(math.degrees(math.atan2(im, re)))
        self.z_curve.setData(xs, zs)
        self.phase_curve.setData(xs, ph)
        self.phase_scatter.setData(xs, ph)
        if xs:
            self.phase_vb.enableAutoRange(axis=pg.ViewBox.YAxis)
            self.phase_vb.autoRange(padding=0.1)

    def _on_sweep_finished(self, state):
        self._sweep_cleanup()
        if self.con_active:
            self._con_on_measured(state)
            return
        self.statusBar().showMessage("扫频完成" if state == 2 else "扫频已停止")

    def _on_sweep_failed(self, msg):
        self._sweep_cleanup()
        if self.con_active:
            self._con_measuring = False
            self.con_confirm.setEnabled(True)
            self._warn("第 %d 串测量出错, 请重测:\n%s" % (self.con_idx, msg))
            return
        self._warn("扫频出错:\n%s" % msg)

    def _sweep_cleanup(self):
        self.btn_start.setEnabled(True)
        self.btn_stop.setEnabled(False)
        self._lock_params(False)
        if self.mb.is_open:
            self.poll_timer.start()   # 恢复温压轮询

    def _lock_params(self, lock):
        for wdg in (self.cmb_sample, self.cmb_gain, self.spn_avg,
                    self.cmb_speed, self.cmb_mode, self.spn_npts,
                    self.btn_connect):
            wdg.setEnabled(not lock)

    # ============================================================ 不拆包测一致性
    def _build_consistency_tab(self):
        w = QtWidgets.QWidget()
        lay = QtWidgets.QHBoxLayout(w)
        lay.setContentsMargins(8, 8, 8, 8); lay.setSpacing(10)

        # 左: 设置 + 向导
        left = QtWidgets.QVBoxLayout(); lay.addLayout(left, 0)

        self.con_setup = QtWidgets.QGroupBox("测试设置")
        f = QtWidgets.QFormLayout(self.con_setup)
        self.con_series = QtWidgets.QSpinBox(); self.con_series.setRange(1, 600); self.con_series.setValue(4)
        self.con_spec = QtWidgets.QLineEdit(); self.con_spec.setPlaceholderText("可选, 如 LFP 280Ah")
        self.con_tol = QtWidgets.QDoubleSpinBox(); self.con_tol.setRange(1, 100); self.con_tol.setValue(15); self.con_tol.setSuffix(" %")
        self.con_rms = QtWidgets.QDoubleSpinBox(); self.con_rms.setRange(0.5, 50); self.con_rms.setValue(5); self.con_rms.setSuffix(" %")
        f.addRow("串数 N *", self.con_series)
        f.addRow("电池规格", self.con_spec)
        f.addRow("Rct 偏差范围", self.con_tol)
        f.addRow("RMS 可用阈值", self.con_rms)
        self.btn_con_start = QtWidgets.QPushButton("开始"); self.btn_con_start.setObjectName("primary")
        self.btn_con_start.clicked.connect(self._con_start)
        f.addRow(self.btn_con_start)
        left.addWidget(self.con_setup)

        gb_w = QtWidgets.QGroupBox("逐串测量向导")
        wl = QtWidgets.QVBoxLayout(gb_w)
        self.con_pinout = PinoutWidget()
        wl.addWidget(self.con_pinout)
        self.con_prompt = QtWidgets.QLabel("填好串数后点\"开始\"。")
        self.con_prompt.setWordWrap(True); self.con_prompt.setStyleSheet("font-size:14px;")
        wl.addWidget(self.con_prompt)
        self.con_confirm = QtWidgets.QPushButton("确认已插好, 开始测量")
        self.con_confirm.setObjectName("primary"); self.con_confirm.setEnabled(False)
        self.con_confirm.clicked.connect(self._con_confirm)
        wl.addWidget(self.con_confirm)
        self.con_status = QtWidgets.QLabel(""); self.con_status.setStyleSheet("color:#0071e3;")
        wl.addWidget(self.con_status)
        self.btn_con_restart = QtWidgets.QPushButton("重新开始")
        self.btn_con_restart.clicked.connect(self._con_start); self.btn_con_restart.setVisible(False)
        wl.addWidget(self.btn_con_restart)
        wl.addStretch(1)
        left.addWidget(gb_w, 1)

        # 右: 结果对比 (表 + 叠加 Nyquist)
        right = QtWidgets.QSplitter(Qt.Vertical); lay.addWidget(right, 1)
        self.con_table = QtWidgets.QTableWidget(0, 6)
        self.con_table.setHorizontalHeaderLabels(["串", "Rs μΩ", "Rct μΩ", "Cdl F", "偏差%", "状态"])
        self.con_table.horizontalHeader().setSectionResizeMode(QtWidgets.QHeaderView.Stretch)
        self.con_table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        right.addWidget(self.con_table)
        self.con_plot = pg.PlotWidget(); self.con_plot.setBackground("w")
        self.con_plot.showGrid(x=True, y=True, alpha=0.25)
        self.con_plot.setLabel("bottom", "串号"); self.con_plot.setLabel("left", "Rct (μΩ)")
        right.addWidget(self.con_plot)
        right.setSizes([360, 300])
        return w

    def _con_start(self):
        self.con_n = self.con_series.value()
        self.con_tol_v = self.con_tol.value()
        self.con_rms_v = self.con_rms.value()
        self.con_results = []           # 每串: dict(k, Rs, Rct, Cdl, L, rms, hz, re, im)
        self.con_idx = 1
        self._con_measuring = False
        self.con_table.setRowCount(0)
        self.con_plot.clear()
        self.btn_con_restart.setVisible(False)
        self.con_setup.setEnabled(False)
        self._con_show_cell()

    def _con_show_cell(self):
        k = self.con_idx
        self.con_pinout.set_state(self.con_n, k)
        self.con_prompt.setText(
            "第 <b>%d/%d</b> 串:把探针插入 <span style='color:#cf222e'>B%d (+)</span> 和 "
            "<span style='color:#0071e3'>B%d (−)</span>,<b>注意正负</b>。插好后点下方按钮。"
            % (k, self.con_n, k - 1, k))
        self.con_confirm.setEnabled(True)
        self.con_status.setText("")

    def _con_confirm(self):
        if not self.mb.is_open:
            self._warn("请先连接设备"); return
        if self._con_measuring:
            return
        self._con_measuring = True
        self.con_active = True
        self.con_confirm.setEnabled(False)
        self.con_status.setText("第 %d 串 测量中…" % self.con_idx)
        self.poll_timer.stop()
        self.s_hz, self.s_re, self.s_im = [], [], []
        codes = DEFAULT_CODES[:self.spn_npts.value()]
        self.sweep_worker = SweepWorker(self.mb, self._build_params(), codes)
        self.sweep_worker.point.connect(self._on_point)
        self.sweep_worker.progress.connect(self._on_progress)
        self.sweep_worker.status.connect(self.statusBar().showMessage)
        self.sweep_worker.finished_.connect(self._on_sweep_finished)
        self.sweep_worker.failed.connect(self._on_sweep_failed)
        self.sweep_worker.start()

    def _con_on_measured(self, state):
        self.con_active = False
        self._con_measuring = False
        # 拟合 + RMS 判定
        a = analyze_eis(self.s_hz, self.s_re, self.s_im)
        e = ecm_fit(self.s_hz, self.s_re, self.s_im, len(a["artifact"])) if len(self.s_hz) >= 8 else None
        if e is None:
            self._warn("第 %d 串数据不可用 (拟合失败 / 缺 scipy / 点数不足),请重插探针重测。" % self.con_idx)
            self.con_confirm.setEnabled(True); return
        if e["rms"] > self.con_rms_v:
            self._warn("第 %d 串数据不可用:RMS=%.1f%% > 阈值 %.1f%%。\n请重新插好探针(检查正负/接触)重测。"
                       % (self.con_idx, e["rms"], self.con_rms_v))
            self.con_confirm.setEnabled(True); return
        self.con_results.append(dict(
            k=self.con_idx, Rs=e["Rs"] * 1e6, Rct=e["Rct"] * 1e6, Cdl=e["Cdl"],
            L=e["L"] * 1e9, rms=e["rms"],
            hz=list(self.s_hz), re=list(self.s_re), im=list(self.s_im)))
        self.con_status.setText("第 %d 串 OK (Rct=%.0fμΩ, RMS=%.1f%%)" % (self.con_idx, e["Rct"] * 1e6, e["rms"]))
        self.con_idx += 1
        if self.con_idx <= self.con_n:
            self._con_show_cell()
        else:
            self._con_finish()

    def _con_finish(self):
        self.con_confirm.setEnabled(False)
        self.con_pinout.set_state(0, -1)
        self.con_setup.setEnabled(True)
        self.btn_con_restart.setVisible(True)
        rct = sorted(r["Rct"] for r in self.con_results)
        med = rct[len(rct) // 2] if rct else 0
        bad = []
        self.con_table.setRowCount(len(self.con_results))
        for row, r in enumerate(self.con_results):
            dev = (r["Rct"] - med) / med * 100 if med else 0
            ok = abs(dev) <= self.con_tol_v
            if not ok:
                bad.append((r["k"], dev))
            cells = ["%d" % r["k"], "%.0f" % r["Rs"], "%.0f" % r["Rct"],
                     "%.2g" % r["Cdl"], "%+.1f" % dev, "OK" if ok else "超差"]
            for c, s in enumerate(cells):
                it = QtWidgets.QTableWidgetItem(s); it.setTextAlignment(Qt.AlignCenter)
                if not ok:
                    it.setBackground(QtGui.QColor("#ffd7d5"))
                self.con_table.setItem(row, c, it)
        # Rct 柱状图 + 中位数线 + 容差带
        self.con_plot.clear()
        ks = [r["k"] for r in self.con_results]; rs = [r["Rct"] for r in self.con_results]
        bar = pg.BarGraphItem(x=ks, height=rs, width=0.6,
                              brushes=[pg.mkBrush("#ff3b30" if abs((v - med) / med * 100) > self.con_tol_v
                                                  else "#0071e3") for v in rs])
        self.con_plot.addItem(bar)
        self.con_plot.addLine(y=med, pen=pg.mkPen("#34c759", width=2, style=Qt.DashLine))
        self.con_plot.addLine(y=med * (1 + self.con_tol_v / 100), pen=pg.mkPen("#ff9500", style=Qt.DotLine))
        self.con_plot.addLine(y=med * (1 - self.con_tol_v / 100), pen=pg.mkPen("#ff9500", style=Qt.DotLine))
        # 汇总弹框
        spec = self.con_spec.text().strip()
        head = "整包 %d 串一致性%s\n中位 Rct=%.0f μΩ, 容差 ±%.0f%%\n\n" % (
            self.con_n, ("(" + spec + ")" if spec else ""), med, self.con_tol_v)
        if bad:
            msg = head + "⚠ 超差串(需关注):\n" + "\n".join(
                "  第 %d 串: Rct 偏离中位 %+.1f%%" % (k, d) for k, d in bad)
            QtWidgets.QMessageBox.warning(self, "一致性结果", msg)
        else:
            QtWidgets.QMessageBox.information(self, "一致性结果", head + "✅ 全部在 ±%.0f%% 内, 一致性良好。" % self.con_tol_v)
        self.statusBar().showMessage("一致性测试完成: %d 串, %d 串超差" % (self.con_n, len(bad)))

    # ---------------------------------------------------------------- 单点
    def start_single(self):
        if not self.mb.is_open:
            self._warn("请先连接设备")
            return
        if self.single_worker and self.single_worker.isRunning():
            return
        code = self.cmb_sp_freq.currentData()
        self.poll_timer.stop()
        self.single_worker = SingleWorker(self.mb, self._build_params(), code)
        self.single_worker.done_.connect(self._on_single_done)
        self.single_worker.failed.connect(self._on_single_failed)
        self.single_worker.status.connect(self.statusBar().showMessage)
        self.single_worker.start()
        self.btn_sp_start.setEnabled(False)
        self.btn_sp_start.setText("测量中…")

    def _on_single_done(self, hz, re, im, vzm):
        mag = math.hypot(re, im)
        phase = math.degrees(math.atan2(im, re))
        self.sp_cards["频率"].setText("%.3f" % hz)
        self.sp_cards["RE"].setText("%.2f" % re)
        self.sp_cards["IM (-Z'')"].setText("%.2f" % im)
        self.sp_cards["|Z|"].setText("%.2f" % mag)
        self.sp_cards["相位"].setText("%.2f" % phase)
        self.sp_cards["VZM"].setText("%.0f" % vzm)
        self._single_cleanup()
        self.statusBar().showMessage("单点测量完成")

    def _on_single_failed(self, msg):
        self._single_cleanup()
        self._warn("单点测量出错:\n%s" % msg)

    def _single_cleanup(self):
        self.btn_sp_start.setEnabled(True)
        self.btn_sp_start.setText("开始单点测量")
        if self.mb.is_open:
            self.poll_timer.start()

    # ---------------------------------------------------------------- CSV
    def export_csv(self):
        if not self.s_hz:
            self._warn("暂无数据可导出")
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "导出 CSV", "JCY8001_EIS.csv", "CSV (*.csv)")
        if not path:
            return
        try:
            with open(path, "w", newline="", encoding="utf-8-sig") as f:
                wr = csv.writer(f)
                wr.writerow(["#", "Frequency(Hz)", "RE(uOhm)", "IM(uOhm)",
                             "|Z|(uOhm)", "Phase(deg)"])
                for i, (hz, re, im) in enumerate(zip(self.s_hz, self.s_re, self.s_im)):
                    wr.writerow([i + 1, "%.4f" % hz, "%.3f" % re, "%.3f" % im,
                                 "%.3f" % math.hypot(re, im),
                                 "%.3f" % math.degrees(math.atan2(im, re))])
            self.statusBar().showMessage("已导出: %s" % path)
        except Exception as e:
            self._warn("导出失败:\n%s" % e)

    # ---------------------------------------------------------------- 校准
    def _open_calibration(self):
        if not self.mb.is_open:
            self._warn("请先连接设备")
            return
        dlg = CalibrationDialog(self.mb, self)
        dlg.exec_()

    # ---------------------------------------------------------------- 工具
    def _warn(self, msg):
        QtWidgets.QMessageBox.warning(self, "JCY8001", msg)

    def closeEvent(self, e):
        try:
            self.stop_sweep()
            if self.sweep_worker:
                self.sweep_worker.wait(1500)
            self.poll_timer.stop()
            self.mb.close()
        except Exception:
            pass
        super().closeEvent(e)


# ============================================================================
#  校准对话框 (0x40D0/D1/D2 = 10/5/1Ω 档实际 mΩ)
# ============================================================================
class CalibrationDialog(QtWidgets.QDialog):
    def __init__(self, mb: Modbus, parent=None):
        super().__init__(parent)
        self.mb = mb
        self.setWindowTitle("校准")
        self.setFixedWidth(340)
        lay = QtWidgets.QVBoxLayout(self)
        lay.setContentsMargins(20, 20, 20, 16)
        lay.setSpacing(12)

        info = QtWidgets.QLabel("各档实际电阻 (mΩ)")
        info.setObjectName("metricCap")
        lay.addWidget(info)

        form = QtWidgets.QFormLayout()
        form.setSpacing(10)
        self.fields = {}
        for name, reg in [("10 Ω 档", R.CAL_10R), ("5 Ω 档", R.CAL_5R), ("1 Ω 档", R.CAL_1R)]:
            sp = QtWidgets.QSpinBox()
            sp.setRange(0, 65535)
            form.addRow(name, sp)
            self.fields[reg] = sp
        lay.addLayout(form)

        btns = QtWidgets.QHBoxLayout()
        b_read = QtWidgets.QPushButton("读取")
        b_write = QtWidgets.QPushButton("写入")
        b_write.setObjectName("primary")
        b_read.clicked.connect(self._read)
        b_write.clicked.connect(self._write)
        btns.addWidget(b_read)
        btns.addWidget(b_write)
        lay.addLayout(btns)

        self._read()

    def _read(self):
        try:
            for reg, sp in self.fields.items():
                sp.setValue(self.mb.read_holding(reg, 1)[0])
        except Exception as e:
            QtWidgets.QMessageBox.warning(self, "校准", "读取失败:\n%s" % e)

    def _write(self):
        try:
            for reg, sp in self.fields.items():
                self.mb.write_single(reg, sp.value())
            QtWidgets.QMessageBox.information(self, "校准", "写入成功")
        except Exception as e:
            QtWidgets.QMessageBox.warning(self, "校准", "写入失败:\n%s" % e)


# ============================================================================
def main():
    pg.setConfigOption("background", "w")
    pg.setConfigOption("foreground", "#1d1d1f")
    pg.setConfigOptions(antialias=True)

    app = QtWidgets.QApplication(sys.argv)
    app.setStyleSheet(QSS)
    app.setFont(QtGui.QFont("PingFang SC", 10))

    win = MainWindow()
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
