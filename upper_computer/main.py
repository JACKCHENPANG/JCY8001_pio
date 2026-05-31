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
#  主窗口
# ============================================================================
class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("JCY8001 · 电化学阻抗谱上位机")
        self.resize(1280, 820)

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
        self.p_nyq = pg.PlotWidget()
        self.p_nyq.setBackground("w")
        self.p_nyq.showGrid(x=True, y=True, alpha=0.25)
        self.p_nyq.setLabel("bottom", "RE", units="μΩ")
        self.p_nyq.setLabel("left", "IM (-Z'')", units="μΩ")
        self.p_nyq.setAspectLocked(True)
        self.nyq_curve = self.p_nyq.plot(
            [], [], pen=pg.mkPen(COL_NYQ, width=2),
            symbol="o", symbolSize=8,
            symbolBrush=COL_NYQ, symbolPen=pg.mkPen("w", width=1.2))
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
        # Nyquist
        self.nyq_curve.setData(self.s_re, self.s_im)
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
        self.statusBar().showMessage("扫频完成" if state == 2 else "扫频已停止")

    def _on_sweep_failed(self, msg):
        self._sweep_cleanup()
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
