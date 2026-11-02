#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
STM32F103 DC UPS Windows 上位机.

通过 Type-C 的 USB CDC 虚拟串口与 UPS 通信，复用 ups_monitor.py 的
UpsClient（XSTATUS / XCONFIG / XSET / XSAVE / XSHUTDOWN ... JSON 协议）。

运行：
    python host/ups_gui.py
    python host/ups_gui.py --port COM23

打包：
    python -m PyInstaller --onefile --windowed --name STM32-UPS-Monitor \
        --icon host/ups.ico --add-data "host/ups.ico;." host/ups_gui.py
"""

from __future__ import annotations

import argparse
import csv
import ctypes
import datetime as dt
import json
import os
import queue
import sys
import threading
import time
import tkinter as tk
from collections import deque
from tkinter import filedialog, font as tkfont, messagebox, ttk
from typing import Any, Dict, List, Optional, Tuple

# 复用 CLI 的协议实现，避免两份解析代码。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

IMPORT_ERROR: Optional[Exception] = None
try:
    import serial
    from serial.tools import list_ports

    from ups_monitor import UpsClient
except Exception as _exc:  # pragma: no cover - 只在缺少依赖时触发
    IMPORT_ERROR = _exc


APP_TITLE = "STM32 UPS 上位机"
APP_VERSION = "1.0.0"

CARD_BG = "#ffffff"
PAGE_BG = "#f3f4f6"
TEXT = "#111827"
MUTED = "#6b7280"
BORDER = "#e5e7eb"
ACCENT = "#2563eb"
OK = "#15803d"
WARN = "#c2410c"
ERROR = "#b91c1c"

STATE_UI = {
    "boot": ("启动中", "#4b5563"),
    "online": ("市电供电", "#15803d"),
    "battery": ("电池供电", "#c2410c"),
    "low_battery": ("电量低", "#b91c1c"),
    "shutdown": ("输出已切断", "#7f1d1d"),
    "fault": ("故障", "#991b1b"),
}

FAULT_TEXT = {
    1 << 0: "电池硬欠压",
    1 << 1: "电池过压",
    1 << 2: "电池未连接",
    1 << 3: "过流",
    1 << 4: "过温",
    1 << 5: "输出欠压",
    1 << 6: "输出过压",
    1 << 7: "输入过压",
    1 << 8: "传感器故障",
    1 << 9: "充电器故障",
    1 << 10: "内部故障",
}

WARN_TEXT = {
    1 << 0: "电池过压",
    1 << 1: "温度禁止充电",
    1 << 2: "温度降额",
    1 << 3: "充电器无状态",
    1 << 4: "电流传感器不可用",
    1 << 5: "温度传感器不可用",
    1 << 6: "输出传感器不可用",
    1 << 7: "配置未保存",
    1 << 8: "电池未连接",
    1 << 9: "输入偏低",
    1 << 10: "充电过流",
    1 << 11: "电池采样异常",
    1 << 12: "NVM 写入失败",
}

ERROR_TEXT = {
    "test_not_supported": "未安装 AC_CUT 硬件，无法执行电池自检。",
    "test_busy": "已有自检正在进行。",
    "no_ac": "当前没有市电，不能开始自检。",
    "no_battery": "未检测到电池。",
    "battery_low": "电量过低，不能开始自检。",
    "not_ready": "设备当前状态不允许该操作。",
    "nvm_write_failed": "NVM 写入失败。",
    "unknown_key": "未知配置键。",
    "not_an_integer": "值必须是十进制整数。",
    "invalid_value": "配置值越界或前后约束不满足。",
    "time_out_of_range": "时间超出允许范围。",
    "bad_seconds": "关机延迟参数错误。",
    "bad_restore": "恢复延迟参数错误。",
    "unknown_command": "设备不支持该命令。",
}

# (键, 中文名, 单位, 最小值, 最大值, 类型)
CONFIG_GROUPS: List[Tuple[str, List[Tuple[str, str, str, int, int, str]]]] = [
    ("电池与容量", [
        ("cells", "串联节数", "S", 1, 6, "int"),
        ("capacity_mah", "额定容量", "mAh", 500, 60000, "int"),
        ("low_pct", "低电量告警", "%", 1, 99, "int"),
        ("critical_pct", "临界电量", "%", 0, 98, "int"),
        ("cutoff_grace_s", "临界后切断延时", "s", 0, 3600, "int"),
        ("ac_restore_s", "市电恢复确认", "s", 1, 60, "int"),
    ]),
    ("单体电压阈值", [
        ("cell_nominal_mv", "单体标称", "mV", 2500, 4500, "int"),
        ("cell_full_mv", "单体充满", "mV", 3000, 4500, "int"),
        ("cell_empty_mv", "单体放空", "mV", 2000, 4000, "int"),
        ("cell_low_mv", "放电低电压", "mV", 2000, 4000, "int"),
        ("cell_critical_mv", "放电临界", "mV", 2000, 4000, "int"),
        ("cell_ov_mv", "单体过压", "mV", 3000, 5000, "int"),
    ]),
    ("电压校准", [
        ("vbat_scale", "电池采样增益", "×/1000", 500, 2000, "int"),
        ("vbat_offset", "电池采样偏置", "mV", -5000, 5000, "int"),
        ("vin_scale", "输入采样增益", "×/1000", 500, 2000, "int"),
        ("vin_offset", "输入采样偏置", "mV", -5000, 5000, "int"),
        ("vout_scale", "输出采样增益", "×/1000", 500, 2000, "int"),
        ("vout_offset", "输出采样偏置", "mV", -5000, 5000, "int"),
    ]),
    ("电流与负载", [
        ("shunt_mohm", "INA219 分流电阻", "mΩ", 0, 1000, "int"),
        ("ina_invert", "电流方向反转", "", 0, 1, "bool"),
        ("max_charge_ma", "充电过流阈值", "mA", 0, 20000, "int"),
        ("max_discharge_ma", "放电过流阈值", "mA", 0, 30000, "int"),
        ("max_load_ma", "输出额定电流", "mA", 0, 30000, "int"),
        ("assumed_load_ma", "假设负载", "mA", 0, 30000, "int"),
    ]),
    ("输入保护", [
        ("vin_present_mv", "市电存在阈值", "mV", 0, 60000, "int"),
        ("vin_nominal_mv", "输入标称电压", "mV", 0, 60000, "int"),
        ("vin_max_mv", "输入过压阈值", "mV", 0, 60000, "int"),
        ("nominal_freq_x10", "Q1 回报频率", "0.1 Hz", 450, 650, "int"),
    ]),
    ("输出保护", [
        ("vout_nominal_mv", "输出标称电压", "mV", 0, 60000, "int"),
        ("vout_uv_mv", "输出欠压故障", "mV", 0, 60000, "int"),
        ("vout_ov_mv", "输出过压故障", "mV", 0, 60000, "int"),
        ("auto_recover", "故障后自动重试", "", 0, 1, "bool"),
    ]),
    ("温度保护", [
        ("charge_max_temp", "充电最高温度", "0.1 °C", -400, 1000, "int"),
        ("discharge_min_temp", "放电最低温度", "0.1 °C", -400, 1000, "int"),
        ("discharge_max_temp", "放电最高温度", "0.1 °C", -400, 1000, "int"),
        ("temp_fault", "过温切断", "0.1 °C", -400, 1000, "int"),
        ("temp_offset", "NTC 校准", "0.1 °C", -200, 200, "int"),
        ("ntc_enabled", "启用 NTC", "", 0, 1, "bool"),
    ]),
    ("其他", [
        ("beeper_enabled", "蜂鸣器使能", "", 0, 1, "bool"),
    ]),
]

CONFIG_FIELDS: Dict[str, Tuple[str, str, int, int, str]] = {}
for _group, _fields in CONFIG_GROUPS:
    for _key, _label, _unit, _lo, _hi, _kind in _fields:
        CONFIG_FIELDS[_key] = (_label, _unit, _lo, _hi, _kind)


# --------------------------------------------------------------------------
# 小工具
# --------------------------------------------------------------------------

def resource_path(name: str) -> str:
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)


def enable_dpi_awareness() -> None:
    if sys.platform != "win32":
        return
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass


def settings_path() -> str:
    base = os.environ.get("APPDATA") or os.path.expanduser("~")
    return os.path.join(base, "STM32-UPS-Monitor", "settings.json")


def load_settings() -> dict:
    try:
        with open(settings_path(), "r", encoding="utf-8") as fh:
            data = json.load(fh)
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def save_settings(data: dict) -> None:
    try:
        path = settings_path()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(data, fh, ensure_ascii=False, indent=2)
    except Exception:
        pass


def fmt_runtime(seconds: Any) -> str:
    try:
        seconds = int(seconds)
    except (TypeError, ValueError):
        return "--"
    if seconds <= 0:
        return "--"
    hours, rem = divmod(seconds, 3600)
    minutes, secs = divmod(rem, 60)
    if hours:
        return f"{hours} 小时 {minutes:02d} 分"
    if minutes:
        return f"{minutes} 分 {secs:02d} 秒"
    return f"{secs} 秒"


def fmt_voltage(mv: Any) -> str:
    try:
        return f"{int(mv) / 1000.0:.2f} V"
    except (TypeError, ValueError):
        return "--"


def fmt_current(ma: Any) -> str:
    try:
        amps = int(ma) / 1000.0
    except (TypeError, ValueError):
        return "--"
    return f"{amps:+.2f} A"


def fmt_temp(temp_x10: Any) -> str:
    try:
        value = int(temp_x10)
    except (TypeError, ValueError):
        return "--"
    if value == 9999:
        return "--"
    return f"{value / 10.0:.1f} °C"


def bit_text(mask: Any, table: Dict[int, str]) -> str:
    try:
        mask = int(mask)
    except (TypeError, ValueError):
        return "未知"
    if not mask:
        return "无"
    names = [text for bit, text in table.items() if mask & bit]
    known = 0
    for bit in table:
        if mask & bit:
            known |= bit
    if mask & ~known:
        names.append(f"0x{mask & ~known:04X}")
    return "、".join(names)


def nice_error(raw: str) -> str:
    key = raw.strip().split()[0] if raw.strip() else raw
    text = ERROR_TEXT.get(key)
    return f"{text}（{raw}）" if text else raw


class Theme:
    def __init__(self, root: tk.Tk) -> None:
        families = set(tkfont.families(root))
        family = "Microsoft YaHei UI"
        for candidate in ("Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI", "Arial"):
            if candidate in families:
                family = candidate
                break
        self.family = family
        self.base = (family, 10)
        self.small = (family, 9)
        self.small_bold = (family, 9, "bold")
        self.bold = (family, 10, "bold")
        self.title = (family, 11, "bold")
        self.value = (family, 18, "bold")
        self.soc = (family, 26, "bold")
        self.state = (family, 16, "bold")
        self.big = (family, 13, "bold")


# --------------------------------------------------------------------------
# 曲线
# --------------------------------------------------------------------------

class HistoryChart(tk.Canvas):
    def __init__(
        self,
        parent: tk.Widget,
        theme: Theme,
        title: str,
        series: Dict[str, Dict[str, str]],
        y_range: Optional[Tuple[float, float]] = None,
        max_points: int = 240,
        height: int = 220,
    ) -> None:
        super().__init__(
            parent,
            bg=CARD_BG,
            height=height,
            highlightthickness=1,
            highlightbackground=BORDER,
            bd=0,
        )
        self.theme = theme
        self.chart_title = title
        self.series = series
        self.y_range = y_range
        self.samples: deque = deque(maxlen=max_points)
        self._pending = False
        self.bind("<Configure>", lambda _e: self._schedule())

    def add_sample(self, when: float, values: Dict[str, Optional[float]]) -> None:
        if not any(values.get(key) is not None for key in self.series):
            return
        self.samples.append((when, dict(values)))
        self._schedule()

    def clear(self) -> None:
        self.samples.clear()
        self._schedule()

    def _schedule(self) -> None:
        if not self._pending:
            self._pending = True
            self.after(30, self.redraw)

    def redraw(self) -> None:
        self._pending = False
        self.delete("all")
        width, height = self.winfo_width(), self.winfo_height()
        if width < 80 or height < 60:
            return

        left, right, top, bottom = 56, 16, 38, 26
        plot_w = width - left - right
        plot_h = height - top - bottom

        self.create_text(
            12, 10, text=self.chart_title, anchor="nw", fill=TEXT, font=self.theme.title
        )

        legend_x = width - right
        for name in reversed(list(self.series)):
            meta = self.series[name]
            item = self.create_text(
                legend_x, 15, text=meta["label"], anchor="ne",
                fill=MUTED, font=self.theme.small,
            )
            box = self.bbox(item)
            if box:
                self.create_rectangle(
                    box[0] - 22, box[1] + 4, box[0] - 13, box[1] + 13,
                    fill=meta["color"], outline="",
                )
                legend_x = box[0] - 30

        self.create_rectangle(
            left, top, left + plot_w, top + plot_h,
            outline=BORDER, fill="#fbfbfc",
        )

        samples = list(self.samples)
        if len(samples) < 2:
            self.create_text(
                left + plot_w / 2, top + plot_h / 2,
                text="等待数据…", fill=MUTED, font=self.theme.title,
            )
            return

        t0, t1 = samples[0][0], samples[-1][0]
        if t1 - t0 < 1e-3:
            t1 = t0 + 1.0

        values = [
            value
            for _when, row in samples
            for value in row.values()
            if value is not None
        ]
        if not values:
            return

        if self.y_range is not None:
            y_lo, y_hi = self.y_range
        else:
            y_lo, y_hi = min(values), max(values)
            mid = (y_hi + y_lo) / 2.0
            min_span = max(0.2, abs(mid) * 0.02)
            if y_hi - y_lo < min_span:
                y_lo, y_hi = mid - min_span / 2.0, mid + min_span / 2.0
            pad = (y_hi - y_lo) * 0.10
            y_lo -= pad
            y_hi += pad

        y_span = y_hi - y_lo
        decimals = 0 if y_span >= 20 else (2 if y_span < 5 else 1)
        steps = 4
        for index in range(steps + 1):
            y = top + plot_h * index / steps
            value = y_hi - y_span * index / steps
            self.create_line(left, y, left + plot_w, y, fill="#eceef1")
            self.create_text(
                left - 8, y, text=f"{value:.{decimals}f}", anchor="e",
                fill=MUTED, font=self.theme.small,
            )

        span = t1 - t0
        for fraction, label, anchor in (
            (0.0, f"-{int(span)}s", "nw"),
            (0.5, f"-{int(span / 2)}s", "n"),
            (1.0, "现在", "ne"),
        ):
            self.create_text(
                left + plot_w * fraction, top + plot_h + 8,
                text=label, anchor=anchor, fill=MUTED, font=self.theme.small,
            )

        def x_of(when: float) -> float:
            return left + (when - t0) / span * plot_w

        def y_of(value: float) -> float:
            return top + (y_hi - value) / (y_hi - y_lo) * plot_h

        for name, meta in self.series.items():
            points: List[float] = []
            for when, row in samples:
                value = row.get(name)
                if value is None:
                    continue
                points.extend((x_of(when), y_of(value)))
            if len(points) >= 4:
                self.create_line(*points, fill=meta["color"], width=2)
            if len(points) >= 2:
                self.create_oval(
                    points[-2] - 3, points[-1] - 3, points[-2] + 3, points[-1] + 3,
                    fill=meta["color"], outline=CARD_BG,
                )


# --------------------------------------------------------------------------
# 数据卡片
# --------------------------------------------------------------------------

class Tile(tk.Frame):
    def __init__(self, parent: tk.Widget, theme: Theme, title: str, unit: str = "") -> None:
        super().__init__(
            parent, bg=CARD_BG, highlightbackground=BORDER, highlightthickness=1, bd=0
        )
        self.unit = unit
        self.title_label = tk.Label(
            self, text=title, bg=CARD_BG, fg=MUTED, font=theme.small, anchor="w"
        )
        self.title_label.pack(fill="x", padx=12, pady=(7, 0))
        self.sub_label = tk.Label(
            self, text="", bg=CARD_BG, fg=MUTED, font=theme.small, anchor="w"
        )
        self.sub_label.pack(fill="x", padx=12, pady=(0, 7), side="bottom")
        self.value_label = tk.Label(
            self, text="--", bg=CARD_BG, fg=TEXT, font=theme.value, anchor="w"
        )
        self.value_label.pack(fill="both", padx=12, expand=True)

    def set(self, value: str, sub: str = "", color: str = TEXT) -> None:
        self.value_label.configure(text=value, fg=color)
        self.sub_label.configure(text=sub)


# --------------------------------------------------------------------------
# 串口/协议线程
# --------------------------------------------------------------------------

class UpsWorker(threading.Thread):
    """所有串口访问都在这个线程里，UI 只通过 queue 交互。"""

    def __init__(self, events: "queue.Queue[dict]") -> None:
        super().__init__(daemon=True, name="ups-worker")
        self.events = events
        self.commands: "queue.Queue[dict]" = queue.Queue()
        self._lock = threading.Lock()
        self._target: Optional[str] = None
        self._interval = 1.0
        self._auto_reconnect = True
        self._stop = threading.Event()
        self.client: Optional["UpsClient"] = None
        self._timeouts = 0

    # -- 线程安全的设置 ---------------------------------------------------
    def set_target(self, port: Optional[str]) -> None:
        with self._lock:
            self._target = port
        if port is None:
            self.commands.put({"name": "disconnect"})

    def get_target(self) -> Optional[str]:
        with self._lock:
            return self._target

    def set_interval(self, seconds: float) -> None:
        with self._lock:
            self._interval = max(0.5, min(10.0, float(seconds)))

    def get_interval(self) -> float:
        with self._lock:
            return self._interval

    def set_auto_reconnect(self, enabled: bool) -> None:
        with self._lock:
            self._auto_reconnect = bool(enabled)

    def stop(self) -> None:
        self._stop.set()

    # -- 内部 -------------------------------------------------------------
    def _emit(self, kind: str, **payload: Any) -> None:
        payload["type"] = kind
        self.events.put(payload)

    def _log(self, message: str, level: str = "info") -> None:
        self._emit("log", message=message, level=level)

    def _close_client(self, reason: str, retry: bool = True) -> None:
        if self.client is not None:
            try:
                self.client.close()
            except Exception:
                pass
            self.client = None
        if retry and self.get_target():
            self._emit("disconnected", reason=reason, retrying=True)
        else:
            with self._lock:
                self._target = None
            self._emit("disconnected", reason=reason, retrying=False)

    def _open(self, port: str) -> bool:
        self._emit("connecting", port=port)
        try:
            self.client = UpsClient(port)
            ident = self.client.identify()
            config = self.client.config()
            status = self.client.status()
        except Exception as exc:
            try:
                if self.client is not None:
                    self.client.close()
            except Exception:
                pass
            self.client = None
            self._emit("connect_failed", port=port, error=str(exc))
            return False

        self._timeouts = 0
        self._emit("connected", port=port, ident=ident)
        self._emit("config", config=config)
        self._emit("status", status=status)
        return True

    def _execute(self, command: dict) -> None:
        name = command.get("name", "")
        if self.client is None:
            return
        try:
            if name == "disconnect":
                self._close_client("已断开", retry=False)
                return

            if name in ("status", "config", "identify"):
                if name == "status":
                    self._emit("status", status=self.client.status())
                elif name == "config":
                    self._emit("config", config=self.client.config())
                else:
                    self._emit("identity", ident=self.client.identify())
                return

            if name == "set":
                key, value = command["key"], str(command["value"])
                self._log(f"→ XSET {key}={value}", "proto")
                response = self.client.set_value(key, value)
                self._log(f"← {json.dumps(response, ensure_ascii=False)}", "proto")
                self._emit("response", action=name, response=response)
                return

            if name == "apply":
                results = []
                for key, value in command["values"].items():
                    try:
                        self._log(f"→ XSET {key}={value}", "proto")
                        self.client.set_value(key, str(value))
                        results.append({"key": key, "ok": True})
                    except Exception as exc:
                        results.append({"key": key, "ok": False, "error": str(exc)})
                        self._log(f"配置 {key} 失败：{exc}", "error")
                self._emit("apply_result", results=results)
                self._emit("config", config=self.client.config())
                return

            if name == "save":
                self._log("→ XSAVE", "proto")
                response = self.client.save()
                self._log(f"← {json.dumps(response, ensure_ascii=False)}", "proto")
                self._emit("response", action=name, response=response)
                self._emit("config", config=self.client.config())
                return

            if name == "reset":
                self._log("→ XRESET", "proto")
                response = self.client.reset()
                self._log(f"← {json.dumps(response, ensure_ascii=False)}", "proto")
                self._emit("response", action=name, response=response)
                self._emit("config", config=self.client.config())
                self._emit("status", status=self.client.status())
                return

            if name == "shutdown":
                seconds = int(command.get("seconds", 0))
                restore = int(command.get("restore", 0))
                if restore:
                    text = f"XSHUTDOWN {seconds} {restore}"
                else:
                    text = f"XSHUTDOWN {seconds}"
                self._log(f"→ {text}", "proto")
                response = self.client.shutdown_after(seconds, restore)
                self._log(f"← {json.dumps(response, ensure_ascii=False)}", "proto")
                self._emit("response", action=name, response=response)
                self._emit("status", status=self.client.status())
                return

            if name == "cancel":
                self._log("→ XCANCEL", "proto")
                response = self.client.cancel_shutdown()
                self._log(f"← {json.dumps(response, ensure_ascii=False)}", "proto")
                self._emit("response", action=name, response=response)
                self._emit("status", status=self.client.status())
                return

            if name == "test":
                duration = str(command.get("duration", "10"))
                self._log(f"→ XTEST {duration}", "proto")
                response = self.client.test(duration)
                self._log(f"← {json.dumps(response, ensure_ascii=False)}", "proto")
                self._emit("response", action=name, response=response)
                return

            if name == "beep":
                self._log("→ XBEEP", "proto")
                response = self.client.beep()
                self._log(f"← {json.dumps(response, ensure_ascii=False)}", "proto")
                self._emit("response", action=name, response=response)
                self._emit("status", status=self.client.status())
                return

        except (serial.SerialException, OSError) as exc:
            self._emit("error", source="command", action=name, error=str(exc))
            self._close_client(str(exc), retry=self._auto_reconnect)
        except Exception as exc:
            self._emit("error", source="command", action=name, error=str(exc))

    def run(self) -> None:  # pragma: no cover - 线程行为靠 smoke test 覆盖
        while not self._stop.is_set():
            port = self.get_target()
            if not port:
                if self._stop.wait(0.2):
                    break
                continue

            if self.client is None:
                if not self._open(port):
                    if not self._auto_reconnect:
                        with self._lock:
                            if self._target == port:
                                self._target = None
                        self._emit("disconnected", reason="连接失败", retrying=False)
                        continue
                    if self._stop.wait(2.0):
                        break
                    continue

            deadline = time.monotonic() + self.get_interval()
            while not self._stop.is_set():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                try:
                    command = self.commands.get(timeout=min(0.2, max(0.02, remaining)))
                except queue.Empty:
                    continue
                self._execute(command)
                deadline = time.monotonic() + self.get_interval()

            if self._stop.is_set() or self.client is None:
                continue

            try:
                status = self.client.status()
                self._timeouts = 0
                self._emit("status", status=status)
            except TimeoutError as exc:
                self._timeouts += 1
                self._emit("error", source="poll", action="status", error=str(exc))
                if self._timeouts >= 3:
                    self._close_client("连续 3 次无响应", retry=self._auto_reconnect)
                    self._stop.wait(1.0)
            except (serial.SerialException, OSError) as exc:
                self._close_client(str(exc), retry=self._auto_reconnect)
                self._stop.wait(1.0)
            except Exception as exc:
                self._emit("error", source="poll", action="status", error=str(exc))


# --------------------------------------------------------------------------
# 主窗口
# --------------------------------------------------------------------------

class UpsApp:
    def __init__(self, args: Optional[argparse.Namespace] = None) -> None:
        self.args = args
        self.settings = load_settings()
        self.root = tk.Tk()
        self.root.title(f"{APP_TITLE} v{APP_VERSION}")
        self.root.configure(bg=PAGE_BG)
        try:
            icon = resource_path("ups.ico")
            if os.path.exists(icon):
                self.root.iconbitmap(default=icon)
        except Exception:
            pass

        self.theme = Theme(self.root)
        self._apply_style()

        self.events: "queue.Queue[dict]" = queue.Queue()
        self.worker = UpsWorker(self.events)
        self.worker.set_auto_reconnect(bool(self.settings.get("auto_reconnect", True)))
        self.worker.start()

        self.connected = False
        self.target_port: Optional[str] = None
        self.session_port: Optional[str] = None
        self.identity: Dict[str, Any] = {}
        self.last_status: Optional[dict] = None
        self.status_count = 0
        self.previous_state: Optional[str] = None
        self.config_loaded: Dict[str, int] = {}
        self.config_cache: Dict[str, int] = {}
        self.connected_at: Optional[float] = None
        self.csv_file = None
        self.csv_writer = None
        self.csv_path = ""
        self.exit_code = 0
        self._closing = False
        self._connecting_port: Optional[str] = None
        self._last_connect_error: Optional[str] = None
        self._port_descriptions: Dict[str, str] = {}

        self.port_var = tk.StringVar(value=str(self.settings.get("port", "")))
        self.interval_var = tk.StringVar(value=str(self.settings.get("interval", 1.0)))
        self.auto_reconnect_var = tk.BooleanVar(
            value=bool(self.settings.get("auto_reconnect", True))
        )
        self.ring_var = tk.BooleanVar(value=bool(self.settings.get("ring", False)))
        self.proto_var = tk.BooleanVar(value=bool(self.settings.get("show_proto", False)))
        self.autoscroll_var = tk.BooleanVar(value=True)
        self.csv_var = tk.BooleanVar(value=False)
        self.delay_var = tk.StringVar(value="60")
        self.restore_var = tk.StringVar(value="300")

        self.status_var = tk.StringVar(value="未连接")
        self.updated_var = tk.StringVar(value="最后更新：--")
        self.dirty_var = tk.StringVar(value="配置：未读取")
        self.beeper_var = tk.StringVar(value="蜂鸣器：--")
        self.csv_label_var = tk.StringVar(value="未记录")

        self.config_vars: Dict[str, tk.StringVar] = {}
        self.config_widgets: Dict[str, tk.Widget] = {}
        self.device_widgets: List[tk.Widget] = []

        self._build_ui()
        self._bind_events()
        self._restore_geometry()
        self.refresh_ports(initial=True)
        self._set_connected_ui(False)

        if getattr(self.args, "port", None):
            self.port_var.set(self.args.port)
            if not getattr(self.args, "smoke_test", False):
                self.root.after(300, self.toggle_connection)

        self.root.after(100, self._drain_events)
        self.root.after(5000, self._auto_refresh_ports)

        tab = getattr(self.args, "tab", None)
        if tab:
            self.select_tab(tab)

        if getattr(self.args, "smoke_test", False):
            self._start_smoke_test(getattr(self.args, "smoke_seconds", 8.0))

    # -- 外观 -------------------------------------------------------------
    def _apply_style(self) -> None:
        style = ttk.Style(self.root)
        try:
            if "vista" in style.theme_names():
                style.theme_use("vista")
        except Exception:
            pass
        style.configure("TButton", font=self.theme.base)
        style.configure("TNotebook", background=PAGE_BG, borderwidth=0)
        style.configure("TNotebook.Tab", font=self.theme.base, padding=(18, 7))
        style.configure("TLabel", background=PAGE_BG, foreground=TEXT, font=self.theme.base)
        style.configure("TFrame", background=PAGE_BG)
        style.configure("TLabelframe", background=CARD_BG)
        style.configure("TLabelframe.Label", background=PAGE_BG, foreground=TEXT,
                        font=self.theme.bold)

    def _restore_geometry(self) -> None:
        screen_w = self.root.winfo_screenwidth()
        screen_h = self.root.winfo_screenheight()
        max_w = max(900, screen_w - 40)
        max_h = max(600, screen_h - 140)
        width = min(1280, max_w)
        height = min(800, max_h)
        x = max(10, (screen_w - width) // 2)
        y = max(10, (screen_h - height) // 3)

        geometry = self.settings.get("geometry")
        if isinstance(geometry, str):
            try:
                size, pos = geometry.split("+", 1)
                w, h = (int(v) for v in size.split("x"))
                px, py = (int(v) for v in pos.split("+")[:2])
                width = min(max(w, 900), max_w)
                height = min(max(h, 600), max_h)
                x = max(0, min(px, max(0, screen_w - width - 10)))
                y = max(0, min(py, max(0, screen_h - height - 70)))
            except Exception:
                pass
        self.root.geometry(f"{width}x{height}+{x}+{y}")
        self.root.minsize(min(1040, max_w), min(640, max_h))

    # -- 构建界面 ---------------------------------------------------------
    def _build_ui(self) -> None:
        toolbar = tk.Frame(self.root, bg=CARD_BG)
        toolbar.pack(side="top", fill="x")
        bar = tk.Frame(toolbar, bg=CARD_BG, padx=12, pady=9)
        bar.pack(fill="x")

        tk.Label(bar, text="端口", bg=CARD_BG, fg=MUTED,
                 font=self.theme.small).pack(side="left")
        self.port_combo = ttk.Combobox(
            bar, textvariable=self.port_var, width=10, font=self.theme.base
        )
        self.port_combo.pack(side="left", padx=(6, 4))
        self.port_combo.bind("<Return>", lambda _e: self.toggle_connection())

        ttk.Button(bar, text="刷新", width=6, command=self.refresh_ports).pack(side="left")
        self.connect_btn = ttk.Button(
            bar, text="连接", width=8, command=self.toggle_connection
        )
        self.connect_btn.pack(side="left", padx=(8, 0))

        ttk.Separator(bar, orient="vertical").pack(side="left", fill="y", padx=12)

        tk.Label(bar, text="刷新间隔", bg=CARD_BG, fg=MUTED,
                 font=self.theme.small).pack(side="left")
        self.interval_spin = ttk.Spinbox(
            bar, from_=0.5, to=10.0, increment=0.5, width=5,
            textvariable=self.interval_var, command=self._interval_changed,
        )
        self.interval_spin.pack(side="left", padx=(6, 2))
        self.interval_spin.bind("<Return>", lambda _e: self._interval_changed())
        self.interval_spin.bind("<FocusOut>", lambda _e: self._interval_changed())
        tk.Label(bar, text="秒", bg=CARD_BG, fg=MUTED,
                 font=self.theme.small).pack(side="left", padx=(0, 12))

        tk.Checkbutton(
            bar, text="自动重连", variable=self.auto_reconnect_var,
            bg=CARD_BG, fg=TEXT, activebackground=CARD_BG, activeforeground=TEXT,
            selectcolor=CARD_BG, font=self.theme.base,
            command=lambda: self.worker.set_auto_reconnect(self.auto_reconnect_var.get()),
        ).pack(side="left")
        tk.Checkbutton(
            bar, text="告警响铃", variable=self.ring_var,
            bg=CARD_BG, fg=TEXT, activebackground=CARD_BG, activeforeground=TEXT,
            selectcolor=CARD_BG, font=self.theme.base,
        ).pack(side="left", padx=(10, 0))

        self.conn_text = tk.Label(bar, text="未连接", bg=CARD_BG, fg=MUTED,
                                  font=self.theme.bold)
        self.conn_text.pack(side="right")
        self.conn_dot = tk.Canvas(bar, width=12, height=12, bg=CARD_BG,
                                  highlightthickness=0, bd=0)
        self.conn_dot.pack(side="right", padx=(0, 6))
        self._draw_dot("#9ca3af")

        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill="both", expand=True, padx=10, pady=(10, 6))

        self._build_monitor_tab()
        self._build_config_tab()
        self._build_actions_tab()
        self._build_log_tab()

        status_bar = tk.Frame(self.root, bg="#e5e7eb")
        status_bar.pack(side="bottom", fill="x")
        tk.Label(
            status_bar, textvariable=self.status_var, bg="#e5e7eb", fg=TEXT,
            font=self.theme.small, anchor="w",
        ).pack(side="left", padx=12, pady=4)
        self.updated_label = tk.Label(
            status_bar, textvariable=self.updated_var, bg="#e5e7eb", fg=MUTED,
            font=self.theme.small, anchor="e",
        )
        self.updated_label.pack(side="right", padx=12, pady=4)

    def _page(self) -> tk.Frame:
        page = tk.Frame(self.notebook, bg=PAGE_BG)
        self.notebook.add(page, text="实时监控")
        return page

    # -- 实时监控页 -------------------------------------------------------
    def _build_monitor_tab(self) -> None:
        outer = self._page()

        self.banner = tk.Frame(outer, bg="#6b7280")
        self.banner.pack(fill="x", pady=(8, 8))
        self._banner_children: List[tk.Widget] = []

        self.state_label = tk.Label(
            self.banner, text="未连接", bg="#6b7280", fg="white",
            font=self.theme.state, anchor="w",
        )
        self.state_label.grid(row=0, column=0, sticky="w", padx=(16, 20), pady=(12, 0))
        self.state_sub = tk.Label(
            self.banner, text="请选择串口并连接", bg="#6b7280", fg="#e5e7eb",
            font=self.theme.small, anchor="w",
        )
        self.state_sub.grid(row=1, column=0, sticky="w", padx=(16, 20), pady=(0, 12))

        self.soc_value = tk.Label(
            self.banner, text="--", bg="#6b7280", fg="white", font=self.theme.soc
        )
        self.soc_value.grid(row=0, column=1, padx=(10, 6), pady=(10, 0))
        self.soc_caption = tk.Label(
            self.banner, text="电量", bg="#6b7280", fg="#e5e7eb", font=self.theme.small
        )
        self.soc_caption.grid(row=1, column=1, sticky="n", pady=(0, 10))

        self.runtime_value = tk.Label(
            self.banner, text="--", bg="#6b7280", fg="white", font=self.theme.big
        )
        self.runtime_value.grid(row=0, column=2, padx=(26, 6), pady=(10, 0))
        self.runtime_caption = tk.Label(
            self.banner, text="预计续航", bg="#6b7280", fg="#e5e7eb",
            font=self.theme.small,
        )
        self.runtime_caption.grid(row=1, column=2, sticky="n", pady=(0, 10))

        self.flags_value = tk.Label(
            self.banner, text="", bg="#6b7280", fg="#e5e7eb",
            font=self.theme.small, anchor="e", justify="right",
        )
        self.flags_value.grid(row=0, column=3, rowspan=2, sticky="e",
                              padx=(20, 16), pady=(12, 12))
        self.banner.columnconfigure(3, weight=1)
        self._banner_children = [
            self.banner, self.state_label, self.state_sub, self.soc_value,
            self.soc_caption, self.runtime_value, self.runtime_caption,
            self.flags_value,
        ]

        content = tk.Frame(outer, bg=PAGE_BG)
        content.pack(fill="both", expand=True)
        content.columnconfigure(0, weight=5, uniform="monitor")
        content.columnconfigure(1, weight=5, uniform="monitor")
        content.rowconfigure(0, weight=1)

        left = tk.Frame(content, bg=PAGE_BG)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 5))
        right = tk.Frame(content, bg=PAGE_BG)
        right.grid(row=0, column=1, sticky="nsew", padx=(5, 0))
        right.rowconfigure(0, weight=1)
        right.rowconfigure(1, weight=1)
        right.columnconfigure(0, weight=1)

        tiles = tk.Frame(left, bg=PAGE_BG)
        tiles.pack(fill="both", expand=True, pady=(0, 8))
        for column in range(4):
            tiles.columnconfigure(column, weight=1, uniform="tile")
        for row in range(2):
            tiles.rowconfigure(row, weight=1)

        self.tile_vin = Tile(tiles, self.theme, "输入电压")
        self.tile_vout = Tile(tiles, self.theme, "输出电压")
        self.tile_vbat = Tile(tiles, self.theme, "电池电压")
        self.tile_ibat = Tile(tiles, self.theme, "电池电流")
        self.tile_load = Tile(tiles, self.theme, "负载")
        self.tile_temp = Tile(tiles, self.theme, "温度")
        self.tile_runtime = Tile(tiles, self.theme, "预计续航")
        self.tile_failures = Tile(tiles, self.theme, "市电掉电次数")
        for index, tile in enumerate([
            self.tile_vin, self.tile_vout, self.tile_vbat, self.tile_ibat,
            self.tile_load, self.tile_temp, self.tile_runtime, self.tile_failures,
        ]):
            tile.grid(row=index // 4, column=index % 4, sticky="nsew", padx=4, pady=4)

        flags = tk.Frame(left, bg=CARD_BG, highlightbackground=BORDER,
                         highlightthickness=1, bd=0)
        flags.pack(fill="x", pady=(0, 8))
        self.flags_line = tk.Label(
            flags, text="市电 -- · 输出 -- · 充电 -- · 电流采样 -- · NTC -- · 配置 --",
            bg=CARD_BG, fg=TEXT, font=self.theme.small, anchor="w", justify="left",
        )
        self.flags_line.pack(fill="x", padx=12, pady=8)
        flags.bind(
            "<Configure>",
            lambda e: self.flags_line.configure(wraplength=max(200, e.width - 24)),
        )

        alerts = tk.Frame(left, bg=CARD_BG, highlightbackground=BORDER,
                          highlightthickness=1, bd=0)
        alerts.pack(fill="x")
        self.fault_label = tk.Label(
            alerts, text="故障：--", bg=CARD_BG, fg=MUTED, font=self.theme.small,
            anchor="w", justify="left",
        )
        self.fault_label.pack(fill="x", padx=12, pady=(8, 0))
        self.warn_label = tk.Label(
            alerts, text="告警：--", bg=CARD_BG, fg=MUTED, font=self.theme.small,
            anchor="w", justify="left",
        )
        self.warn_label.pack(fill="x", padx=12, pady=(2, 8))
        alerts.bind(
            "<Configure>",
            lambda e: [
                label.configure(wraplength=max(200, e.width - 24))
                for label in (self.fault_label, self.warn_label)
            ],
        )

        self.chart_voltage = HistoryChart(
            right, self.theme, "电压 (V)",
            {
                "vin": {"label": "输入", "color": "#2563eb"},
                "vout": {"label": "输出", "color": "#0891b2"},
                "vbat": {"label": "电池", "color": "#f59e0b"},
            },
        )
        self.chart_voltage.grid(row=0, column=0, sticky="nsew", pady=(0, 5))
        self.chart_percent = HistoryChart(
            right, self.theme, "电量 / 负载 (%)",
            {
                "soc": {"label": "电量", "color": "#16a34a"},
                "load": {"label": "负载", "color": "#7c3aed"},
            },
            y_range=(0.0, 100.0),
        )
        self.chart_percent.grid(row=1, column=0, sticky="nsew", pady=(5, 0))

    # -- 配置页 -----------------------------------------------------------
    def _build_config_tab(self) -> None:
        page = self._page_config()
        toolbar = tk.Frame(page, bg=PAGE_BG)
        toolbar.pack(fill="x", pady=(8, 8))

        self.read_btn = ttk.Button(toolbar, text="从设备读取", command=self.read_config)
        self.read_btn.pack(side="left")
        self.apply_btn = ttk.Button(toolbar, text="应用修改", command=self.apply_config)
        self.apply_btn.pack(side="left", padx=6)
        self.save_btn = ttk.Button(toolbar, text="写入 NVM", command=self.save_config)
        self.save_btn.pack(side="left")
        self.reset_btn = tk.Button(
            toolbar, text="恢复出厂设置", command=self.reset_config,
            bg="#fee2e2", fg=ERROR, activebackground="#fecaca", activeforeground=ERROR,
            relief="flat", padx=10, pady=3, font=self.theme.base,
        )
        self.reset_btn.pack(side="left", padx=(6, 0))

        self.device_widgets.extend([self.read_btn, self.apply_btn, self.save_btn,
                                    self.reset_btn])

        tk.Label(toolbar, textvariable=self.dirty_var, bg=PAGE_BG, fg=WARN,
                 font=self.theme.bold).pack(side="left", padx=(14, 0))

        hint = tk.Label(
            page,
            text="修改先写入设备 RAM，点“写入 NVM”才会掉电保存；60 秒内无操作设备会自动保存。"
                 "单体阈值必须满足 放空 < 临界 < 低电压 < 充满 < 过压。",
            bg=PAGE_BG, fg=MUTED, font=self.theme.small, anchor="w", justify="left",
        )
        hint.pack(fill="x", pady=(0, 8))
        hint.bind("<Configure>", lambda e: hint.configure(wraplength=max(300, e.width - 8)))

        outer, inner, canvas = self._make_scrollable(page)
        outer.pack(fill="both", expand=True)

        for group_index, (group_title, fields) in enumerate(CONFIG_GROUPS):
            frame = tk.LabelFrame(
                inner, text=group_title, bg=CARD_BG, fg=TEXT,
                font=self.theme.bold, padx=10, pady=6, bd=1, relief="groove",
            )
            frame.grid(row=group_index // 2, column=group_index % 2,
                       sticky="nsew", padx=6, pady=6)
            frame.columnconfigure(0, weight=1)
            for row_index, (key, label, unit, low, high, kind) in enumerate(fields):
                tk.Label(
                    frame, text=label, bg=CARD_BG, fg=TEXT,
                    font=self.theme.base, anchor="w",
                ).grid(row=row_index, column=0, sticky="w", pady=2)
                variable = tk.StringVar()
                self.config_vars[key] = variable
                if kind == "bool":
                    widget = ttk.Combobox(
                        frame, textvariable=variable, values=["关", "开"],
                        state="readonly", width=6, font=self.theme.base,
                    )
                else:
                    widget = ttk.Entry(
                        frame, textvariable=variable, width=9,
                        justify="right", font=self.theme.base,
                    )
                widget.grid(row=row_index, column=1, sticky="e", padx=(8, 4), pady=1)
                tk.Label(
                    frame, text=unit, bg=CARD_BG, fg=MUTED,
                    font=self.theme.small, anchor="w", width=7,
                ).grid(row=row_index, column=2, sticky="w")
                self.config_widgets[key] = widget
                self.device_widgets.append(widget)

        inner.columnconfigure(0, weight=1)
        inner.columnconfigure(1, weight=1)

    def _page_config(self) -> tk.Frame:
        page = tk.Frame(self.notebook, bg=PAGE_BG)
        self.notebook.add(page, text="配置")
        return page

    def _make_scrollable(self, parent: tk.Widget):
        outer = tk.Frame(parent, bg=PAGE_BG)
        canvas = tk.Canvas(outer, bg=PAGE_BG, highlightthickness=0, bd=0)
        scrollbar = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        inner = tk.Frame(canvas, bg=PAGE_BG)
        window = canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=scrollbar.set)
        inner.bind(
            "<Configure>",
            lambda _e: canvas.configure(scrollregion=canvas.bbox("all")),
        )
        canvas.bind(
            "<Configure>",
            lambda e: canvas.itemconfigure(window, width=e.width),
        )

        def on_wheel(event: tk.Event) -> None:
            canvas.yview_scroll(int(-event.delta / 120), "units")

        canvas.bind("<Enter>", lambda _e: canvas.bind_all("<MouseWheel>", on_wheel))
        canvas.bind("<Leave>", lambda _e: canvas.unbind_all("<MouseWheel>"))
        canvas.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")
        return outer, inner, canvas

    # -- 操作页 -----------------------------------------------------------
    def _build_actions_tab(self) -> None:
        page = tk.Frame(self.notebook, bg=PAGE_BG)
        self.notebook.add(page, text="操作")

        identity = tk.LabelFrame(
            page, text="设备信息", bg=CARD_BG, fg=TEXT, font=self.theme.bold,
            padx=12, pady=8, bd=1, relief="groove",
        )
        identity.pack(fill="x", pady=(8, 8))
        self.identity_labels: Dict[str, tk.Label] = {}
        labels = [
            ("device", "设备"), ("mfr", "厂商"), ("fw", "固件"),
            ("proto", "协议"), ("board", "板卡"), ("transport", "传输"),
        ]
        for index, (key, title) in enumerate(labels):
            row, column = divmod(index, 3)
            tk.Label(identity, text=title, bg=CARD_BG, fg=MUTED,
                     font=self.theme.small, anchor="w").grid(
                row=row * 2, column=column, sticky="w", padx=(0, 18), pady=(2, 0)
            )
            value = tk.Label(identity, text="--", bg=CARD_BG, fg=TEXT,
                             font=self.theme.base, anchor="w")
            value.grid(row=row * 2 + 1, column=column, sticky="w",
                       padx=(0, 18), pady=(0, 4))
            self.identity_labels[key] = value
        for column in range(3):
            identity.columnconfigure(column, weight=1)

        cards = tk.Frame(page, bg=PAGE_BG)
        cards.pack(fill="x", pady=(0, 8))
        for column in range(3):
            cards.columnconfigure(column, weight=1, uniform="cards")

        beeper = tk.LabelFrame(cards, text="蜂鸣器", bg=CARD_BG, fg=TEXT,
                               font=self.theme.bold, padx=12, pady=10,
                               bd=1, relief="groove")
        beeper.grid(row=0, column=0, sticky="nsew", padx=(0, 4))
        tk.Label(beeper, textvariable=self.beeper_var, bg=CARD_BG, fg=TEXT,
                 font=self.theme.big, anchor="w").pack(fill="x")
        self.beep_btn = ttk.Button(beeper, text="切换蜂鸣器", command=self.toggle_beeper)
        self.beep_btn.pack(anchor="w", pady=(10, 0))
        self.device_widgets.append(self.beep_btn)

        selftest = tk.LabelFrame(cards, text="电池自检", bg=CARD_BG, fg=TEXT,
                                 font=self.theme.bold, padx=12, pady=10,
                                 bd=1, relief="groove")
        selftest.grid(row=0, column=1, sticky="nsew", padx=4)
        test_row = tk.Frame(selftest, bg=CARD_BG)
        test_row.pack(fill="x")
        for text, duration in (("10 秒", "10"), ("60 秒", "60"), ("到低电量", "low")):
            button = ttk.Button(
                test_row, text=text, width=9,
                command=lambda value=duration: self.run_test(value),
            )
            button.pack(side="left", padx=(0, 4))
            self.device_widgets.append(button)
        tk.Label(
            selftest,
            text="需要 AC_CUT 硬件；没有时设备会拒绝并返回 test_not_supported。",
            bg=CARD_BG, fg=MUTED, font=self.theme.small, anchor="w",
            justify="left", wraplength=330,
        ).pack(fill="x", pady=(10, 0))

        shutdown = tk.LabelFrame(cards, text="计划切断", bg=CARD_BG, fg=TEXT,
                                 font=self.theme.bold, padx=12, pady=10,
                                 bd=1, relief="groove")
        shutdown.grid(row=0, column=2, sticky="nsew", padx=(4, 0))

        tk.Label(shutdown, text="延迟切断", bg=CARD_BG, fg=MUTED,
                 font=self.theme.small).grid(row=0, column=0, sticky="w")
        self.delay_spin = ttk.Spinbox(
            shutdown, from_=0, to=86400, increment=10, width=7,
            textvariable=self.delay_var,
        )
        self.delay_spin.grid(row=0, column=1, sticky="w", padx=(6, 4))
        tk.Label(shutdown, text="秒", bg=CARD_BG, fg=MUTED,
                 font=self.theme.small).grid(row=0, column=2, sticky="w")

        tk.Label(shutdown, text="自动恢复", bg=CARD_BG, fg=MUTED,
                 font=self.theme.small).grid(row=1, column=0, sticky="w", pady=(6, 0))
        self.restore_spin = ttk.Spinbox(
            shutdown, from_=0, to=604800, increment=30, width=7,
            textvariable=self.restore_var,
        )
        self.restore_spin.grid(row=1, column=1, sticky="w", padx=(6, 4), pady=(6, 0))
        tk.Label(shutdown, text="秒（0=不恢复）", bg=CARD_BG, fg=MUTED,
                 font=self.theme.small).grid(row=1, column=2, sticky="w", pady=(6, 0))

        buttons = tk.Frame(shutdown, bg=CARD_BG)
        buttons.grid(row=2, column=0, columnspan=3, sticky="w", pady=(10, 0))
        self.shutdown_btn = tk.Button(
            buttons, text="计划切断输出", command=self.plan_shutdown,
            bg="#b91c1c", fg="white", activebackground="#991b1b",
            activeforeground="white", relief="flat", padx=10, pady=4,
            font=self.theme.base,
        )
        self.shutdown_btn.pack(side="left")
        self.cancel_btn = ttk.Button(buttons, text="取消计划", command=self.cancel_shutdown)
        self.cancel_btn.pack(side="left", padx=(6, 0))
        self.device_widgets.extend([self.delay_spin, self.restore_spin,
                                    self.shutdown_btn, self.cancel_btn])

        notes = tk.LabelFrame(page, text="注意", bg=CARD_BG, fg=TEXT,
                              font=self.theme.bold, padx=12, pady=8,
                              bd=1, relief="groove")
        notes.pack(fill="x")
        tk.Label(
            notes,
            text="· 自检会主动切到电池运行，真实自检需要可选的 AC_CUT 硬件。\n"
                 "· 计划切断倒计时结束后输出关闭；如果本机由 UPS 供电，请先完成本机关机。\n"
                 "· 同一串口不要同时被 NUT、ups_monitor.py 或本程序占用。\n"
                 "· 恢复出厂设置会重置配置、计数器与 SoC 状态，且立即写入 NVM。",
            bg=CARD_BG, fg=MUTED, font=self.theme.base, anchor="nw",
            justify="left", wraplength=900,
        ).pack(fill="both", expand=True)
        notes.bind(
            "<Configure>",
            lambda e: notes.winfo_children()[0].configure(
                wraplength=max(300, e.width - 28)
            ),
        )

    # -- 日志页 -----------------------------------------------------------
    def _build_log_tab(self) -> None:
        page = tk.Frame(self.notebook, bg=PAGE_BG)
        self.notebook.add(page, text="日志")

        toolbar = tk.Frame(page, bg=PAGE_BG)
        toolbar.pack(fill="x", pady=(8, 6))
        tk.Checkbutton(
            toolbar, text="记录到 CSV", variable=self.csv_var,
            bg=PAGE_BG, fg=TEXT, activebackground=PAGE_BG, activeforeground=TEXT,
            selectcolor=CARD_BG, font=self.theme.base, command=self.toggle_csv,
        ).pack(side="left")
        ttk.Button(toolbar, text="选择文件…", command=self.choose_csv).pack(
            side="left", padx=(6, 10)
        )
        tk.Checkbutton(
            toolbar, text="自动滚动", variable=self.autoscroll_var,
            bg=PAGE_BG, fg=TEXT, activebackground=PAGE_BG, activeforeground=TEXT,
            selectcolor=CARD_BG, font=self.theme.base,
        ).pack(side="left")
        tk.Checkbutton(
            toolbar, text="显示协议报文", variable=self.proto_var,
            bg=PAGE_BG, fg=TEXT, activebackground=PAGE_BG, activeforeground=TEXT,
            selectcolor=CARD_BG, font=self.theme.base,
        ).pack(side="left", padx=(10, 0))
        ttk.Button(toolbar, text="清空", width=6, command=self.clear_log).pack(
            side="right"
        )
        ttk.Button(toolbar, text="导出日志…", command=self.export_log).pack(
            side="right", padx=(0, 6)
        )
        tk.Label(toolbar, textvariable=self.csv_label_var, bg=PAGE_BG, fg=MUTED,
                 font=self.theme.small).pack(side="left", padx=(10, 0))

        self.log_text = tk.Text(
            page, bg="#111827", fg="#d1d5db", insertbackground="#d1d5db",
            font=self.theme.small, relief="flat", wrap="word", padx=10, pady=8,
            height=10,
        )
        scrollbar = ttk.Scrollbar(page, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        self.log_text.pack(side="left", fill="both", expand=True)
        self.log_text.tag_configure("info", foreground="#d1d5db")
        self.log_text.tag_configure("ok", foreground="#4ade80")
        self.log_text.tag_configure("warn", foreground="#fbbf24")
        self.log_text.tag_configure("error", foreground="#f87171")
        self.log_text.tag_configure("proto", foreground="#94a3b8")
        self.log_text.configure(state="disabled")

    # -- 事件绑定 ---------------------------------------------------------
    def _bind_events(self) -> None:
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def _draw_dot(self, color: str) -> None:
        self.conn_dot.delete("all")
        self.conn_dot.create_oval(2, 2, 10, 10, fill=color, outline="")

    def _set_indicator(self, color: str, text: str) -> None:
        self._draw_dot(color)
        self.conn_text.configure(text=text, fg=color if color != "#9ca3af" else MUTED)

    # -- 串口列表 ---------------------------------------------------------
    def refresh_ports(self, initial: bool = False) -> None:
        try:
            ports = list(list_ports.comports())
        except Exception as exc:
            self._log(f"枚举串口失败：{exc}", "error")
            return

        names = [port.device for port in ports]
        self._port_descriptions = {
            port.device: (port.description or "") for port in ports
        }
        current = self.port_var.get().strip()
        self.port_combo["values"] = names

        if not current:
            preferred = [
                port.device for port in ports
                if getattr(port, "vid", None) == 0x0483
                and getattr(port, "pid", None) == 0x5740
            ]
            saved = str(self.settings.get("port", "")).strip()
            if saved in names:
                self.port_var.set(saved)
            elif len(preferred) == 1:
                self.port_var.set(preferred[0])
            elif names:
                self.port_var.set(names[0])

    def _auto_refresh_ports(self) -> None:
        if self._closing:
            return
        try:
            if not self.connected and not self.target_port:
                if self.root.focus_get() is not self.port_combo:
                    self.refresh_ports()
        except Exception:
            pass
        self.root.after(5000, self._auto_refresh_ports)

    def select_tab(self, name: str) -> None:
        mapping = {"monitor": 0, "config": 1, "actions": 2, "log": 3}
        index = mapping.get(name.lower())
        if index is not None:
            self.notebook.select(index)

    def current_port(self) -> str:
        return self.port_var.get().strip()

    # -- 连接/断开 --------------------------------------------------------
    def toggle_connection(self) -> None:
        if self.target_port:
            self.target_port = None
            self.worker.set_target(None)
            self._set_indicator("#9ca3af", "已断开")
            self._log("已请求断开", "info")
            return
        port = self.current_port()
        if not port:
            messagebox.showwarning(APP_TITLE, "请先选择或输入串口，例如 COM23。")
            return
        self.target_port = port
        self.session_port = port
        self._set_indicator("#f59e0b", "连接中…")
        self.status_var.set(f"正在连接 {port}…")
        self.connect_btn.configure(text="断开")
        self.worker.set_interval(self._interval_value())
        self.worker.set_target(port)
        self._log(f"正在连接 {port}…", "info")

    def _interval_value(self) -> float:
        try:
            return max(0.5, min(10.0, float(self.interval_var.get())))
        except ValueError:
            self.interval_var.set("1.0")
            return 1.0

    def _interval_changed(self) -> None:
        value = self._interval_value()
        self.worker.set_interval(value)
        self.status_var.set(f"刷新间隔 {value:.1f} 秒")

    # -- 队列分发 ---------------------------------------------------------
    def _drain_events(self) -> None:
        if self._closing:
            return
        try:
            while True:
                event = self.events.get_nowait()
                self._handle_event(event)
        except queue.Empty:
            pass
        except Exception as exc:  # 避免 UI 回调异常导致轮询停止
            self._log(f"界面事件处理异常：{exc}", "error")
        self.root.after(100, self._drain_events)

    def _handle_event(self, event: dict) -> None:
        kind = event.get("type")

        if kind == "status":
            self._update_status(event["status"])
            return

        if kind == "config":
            self._load_config(event["config"])
            return

        if kind == "connected":
            self.connected = True
            self.connected_at = time.monotonic()
            self.session_port = event.get("port")
            self.identity = event.get("ident", {})
            self.last_status = None
            self.status_count = 0
            self.previous_state = None
            self._last_connect_error = None
            self.chart_voltage.clear()
            self.chart_percent.clear()
            self._set_indicator(OK, "已连接")
            self.connect_btn.configure(text="断开")
            self._set_connected_ui(True)
            self._update_identity()
            port = self.session_port or ""
            desc = self._port_descriptions.get(port, "")
            self.status_var.set(
                f"已连接 {port}" + (f" · {desc}" if desc else "") +
                f" · 固件 {self.identity.get('fw', '?')}"
            )
            self._log(
                f"已连接 {port} · {self.identity.get('device', '?')} · "
                f"固件 {self.identity.get('fw', '?')} · "
                f"协议 {self.identity.get('proto', '?')}",
                "ok",
            )
            return

        if kind == "connecting":
            port = event.get("port", "")
            if self._connecting_port != port:
                self._connecting_port = port
                self.status_var.set(f"正在连接 {port}…")
            return

        if kind == "connect_failed":
            port = event.get("port", "")
            error = event.get("error", "")
            self.status_var.set(f"{port} 连接失败：{nice_error(error)}；等待重试")
            if error != self._last_connect_error:
                self._log(f"{port} 连接失败：{error}", "error")
                self._last_connect_error = error
            self._connecting_port = port
            if self.target_port:
                self._set_indicator("#f59e0b", "连接中…")
            return

        if kind == "disconnected":
            self.connected = False
            self.connected_at = None
            retrying = event.get("retrying", False)
            reason = event.get("reason", "")
            self._set_connected_ui(False)
            if retrying and self.target_port:
                self._set_indicator("#f59e0b", "重连中…")
                self.connect_btn.configure(text="断开")
                self.status_var.set(f"连接中断：{nice_error(reason)}；2 秒后重试")
            else:
                self.target_port = None
                self._set_indicator("#9ca3af", "未连接")
                self.connect_btn.configure(text="连接")
                self.status_var.set(f"已断开：{nice_error(reason)}" if reason else "未连接")
            self._log(f"连接断开：{reason}", "warn" if retrying else "info")
            return

        if kind == "log":
            level = event.get("level", "info")
            self._log(event.get("message", ""), level)
            return

        if kind == "apply_result":
            results = event.get("results", [])
            failed = [item for item in results if not item.get("ok")]
            if failed:
                detail = "\n".join(
                    f"{item['key']}: {nice_error(item.get('error', ''))}"
                    for item in failed
                )
                messagebox.showwarning(APP_TITLE, f"部分配置未应用：\n{detail}")
                self.status_var.set(f"{len(failed)} 项配置未应用")
            else:
                count = len(results)
                self.status_var.set(f"已应用 {count} 项配置（尚未写入 NVM）")
                self._log(f"已应用 {count} 项配置到 RAM", "ok")
            return

        if kind == "response":
            action = event.get("action")
            response = event.get("response", {})
            if action == "save":
                self.status_var.set("配置已写入 NVM")
                self._log("配置已写入 NVM", "ok")
            elif action == "reset":
                self.status_var.set("已恢复出厂设置")
                self._log("已恢复出厂设置，配置已写入 NVM", "warn")
            elif action == "beep":
                beeper = bool(response.get("beeper"))
                self.beeper_var.set("蜂鸣器：" + ("开" if beeper else "关"))
                self.status_var.set("蜂鸣器已" + ("开启" if beeper else "关闭"))
            elif action == "shutdown":
                text = f"将在 {response.get('shutdown_in_s', '?')} 秒后切断输出"
                if response.get("restore_in_s"):
                    text += f"，{response['restore_in_s']} 秒后恢复"
                self.status_var.set(text)
                self._log(text, "warn")
            elif action == "cancel":
                self.status_var.set("已取消关机计划")
                self._log("已取消关机计划", "ok")
            elif action == "test":
                self.status_var.set("自检已开始")
                self._log("自检已开始", "ok")
            return

        if kind == "error":
            source = event.get("source")
            action = event.get("action", "")
            error = event.get("error", "")
            self._log(f"{action or '设备'} 错误：{error}", "error")
            if source == "command":
                messagebox.showerror(APP_TITLE, nice_error(error))
            else:
                self.status_var.set(f"通信错误：{nice_error(error)}")
            return

    # -- 状态刷新 ---------------------------------------------------------
    def _update_status(self, status: dict) -> None:
        self.last_status = status
        self.status_count += 1
        now = time.monotonic()
        now_wall = dt.datetime.now()

        soc = self._int(status.get("soc_x10")) / 10.0
        state = str(status.get("state", "boot"))
        state_text, color = STATE_UI.get(state, (state, "#4b5563"))
        if not self.connected:
            state_text, color = "未连接", "#6b7280"
        self._paint_banner(color, state_text)

        self.soc_value.configure(text=f"{soc:.1f}%")
        runtime = fmt_runtime(status.get("runtime_s"))
        self.runtime_value.configure(text=runtime)

        ac = bool(status.get("ac"))
        output = bool(status.get("output"))
        charging = bool(status.get("charging"))
        self.state_sub.configure(
            text=f"{self.session_port or ''} · 市电{'正常' if ac else '缺失'} · "
                 f"输出{'开启' if output else '关闭'} · "
                 f"{'充电中' if charging else ('已充满' if status.get('charge_done') else '未充电')}"
        )

        vin = self._int(status.get("vin_mv"))
        vout = self._int(status.get("vout_mv"))
        vbat = self._int(status.get("vbat_mv"))
        ibat = self._int(status.get("ibat_ma"))
        iout = self._int(status.get("iout_ma"))
        load = self._int(status.get("load_pct"))
        temp = status.get("temp_c_x10")
        cells = max(1, self._int(self.config_cache.get("cells", 2)))

        self.flags_value.configure(
            text=f"输入 {fmt_voltage(vin)}\n输出 {fmt_voltage(vout)}\n"
                 f"电池 {fmt_voltage(vbat)}"
        )

        self.tile_vin.set(fmt_voltage(vin))
        self.tile_vout.set(fmt_voltage(vout))
        self.tile_vbat.set(fmt_voltage(vbat),
                           sub=f"{cells}S · 单体 {vbat / cells / 1000.0:.2f} V")
        self.tile_ibat.set(
            fmt_current(ibat),
            sub="正=充电 / 负=放电" if abs(ibat) > 50 else "待机",
        )
        self.tile_load.set(f"{load}%", sub=f"输出 {fmt_current(iout)}")
        self.tile_temp.set(
            fmt_temp(temp),
            sub="NTC 正常" if status.get("ntc_ok") else "NTC 不可用",
        )
        self.tile_runtime.set(runtime, sub=f"电量 {soc:.1f}%")
        self.tile_failures.set(
            str(self._int(status.get("ac_fail_count"))),
            sub="累计市电掉电次数",
        )

        self.flags_line.configure(
            text=" · ".join([
                f"市电：{'有' if ac else '无'}",
                f"输出：{'开' if output else '关'}",
                f"充电：{'是' if charging else '否'}",
                f"充满：{'是' if status.get('charge_done') else '否'}",
                f"电流采样：{'正常' if status.get('ina_ok') else '不可用'}",
                f"NTC：{'正常' if status.get('ntc_ok') else '不可用'}",
                f"配置：{'有未保存修改' if status.get('config_dirty') else '已保存'}",
            ])
        )

        faults = self._int(status.get("faults"))
        warnings_mask = self._int(status.get("warnings"))
        self.fault_label.configure(
            text="故障：" + bit_text(faults, FAULT_TEXT),
            fg=ERROR if faults else MUTED,
        )
        self.warn_label.configure(
            text="告警：" + bit_text(warnings_mask, WARN_TEXT),
            fg=WARN if warnings_mask else MUTED,
        )

        self.chart_voltage.add_sample(now, {
            "vin": vin / 1000.0,
            "vout": vout / 1000.0,
            "vbat": vbat / 1000.0,
        })
        self.chart_percent.add_sample(now, {
            "soc": soc,
            "load": float(load),
        })

        self.updated_var.set(f"最后更新：{now_wall.strftime('%H:%M:%S')}")

        if state != self.previous_state:
            if self.previous_state is not None:
                message = f"状态变化：{STATE_UI.get(self.previous_state, (self.previous_state,))[0]} → {state_text}"
                level = "warn" if state in ("battery", "low_battery", "fault", "shutdown") else "ok"
                self._log(message, level)
                if self.ring_var.get() and state in ("low_battery", "fault"):
                    try:
                        self.root.bell()
                    except Exception:
                        pass
            self.previous_state = state

        if self.csv_writer is not None:
            self._write_csv(status, soc, now_wall)

    def _paint_banner(self, color: str, text: str) -> None:
        self.banner.configure(bg=color)
        for widget in self._banner_children:
            try:
                widget.configure(bg=color)
            except Exception:
                pass
        self.state_label.configure(text=text)
        self.flags_value.configure(fg="#e5e7eb")

    def _load_config(self, config: dict) -> None:
        for key, variable in self.config_vars.items():
            if key not in config:
                continue
            value = self._int(config[key])
            self.config_loaded[key] = value
            self.config_cache[key] = value
            if CONFIG_FIELDS[key][4] == "bool":
                variable.set("开" if value else "关")
            else:
                variable.set(str(value))
        dirty = bool(config.get("dirty"))
        self.dirty_var.set("配置：有未保存修改" if dirty else "配置：已保存")
        self.beeper_var.set(
            "蜂鸣器：" + ("开" if self._int(
                config.get("beeper_enabled", self.config_cache.get("beeper_enabled", 1))
            ) else "关")
        )
        if self.last_status:
            vbat = self._int(self.last_status.get("vbat_mv"))
            cells = max(1, self._int(self.config_cache.get("cells", 2)))
            self.tile_vbat.set(
                fmt_voltage(vbat),
                sub=f"{cells}S · 单体 {vbat / cells / 1000.0:.2f} V",
            )

    def _update_identity(self) -> None:
        for key, label in self.identity_labels.items():
            label.configure(text=str(self.identity.get(key, "--")))
        if self.last_status:
            self._update_status(self.last_status)

    def _set_connected_ui(self, connected: bool) -> None:
        for widget in self.device_widgets:
            try:
                if isinstance(widget, ttk.Combobox):
                    widget.configure(state="readonly" if connected else "disabled")
                else:
                    widget.configure(state="normal" if connected else "disabled")
            except Exception:
                pass
        if not connected:
            self.dirty_var.set("配置：未连接")
            self.beeper_var.set("蜂鸣器：--")
            self._paint_banner("#6b7280", "未连接")
            self.state_sub.configure(text="请选择串口并连接")
            self.soc_value.configure(text="--")
            self.runtime_value.configure(text="--")
            self.flags_value.configure(text="输入 --\n输出 --\n电池 --")
            self.updated_var.set("最后更新：--")
            self.previous_state = None

    # -- 配置操作 ---------------------------------------------------------
    def read_config(self) -> None:
        if self.connected:
            self.worker.commands.put({"name": "config"})
            self.status_var.set("正在读取配置…")

    def apply_config(self) -> None:
        if not self.connected:
            return
        changes: Dict[str, int] = {}
        errors: List[str] = []
        for key, variable in self.config_vars.items():
            label, _unit, low, high, kind = CONFIG_FIELDS[key]
            raw = variable.get().strip()
            if kind == "bool":
                value = 1 if raw == "开" else 0
            else:
                try:
                    value = int(raw, 10)
                except ValueError:
                    errors.append(f"{label}（{key}）：必须是整数")
                    continue
                if value < low or value > high:
                    errors.append(f"{label}（{key}）：应在 {low}..{high} 之间")
                    continue
            if self.config_loaded.get(key) != value:
                changes[key] = value
        if errors:
            messagebox.showerror(APP_TITLE, "配置未应用：\n\n" + "\n".join(errors))
            return
        if not changes:
            messagebox.showinfo(APP_TITLE, "没有检测到修改。")
            return
        self.worker.commands.put({"name": "apply", "values": changes})
        self.status_var.set(f"正在应用 {len(changes)} 项配置…")

    def save_config(self) -> None:
        if not self.connected:
            return
        if not messagebox.askyesno(
            APP_TITLE, "将当前配置写入设备 NVM（掉电保存）？"
        ):
            return
        self.worker.commands.put({"name": "save"})
        self.status_var.set("正在写入 NVM…")

    def reset_config(self) -> None:
        if not self.connected:
            return
        if not messagebox.askyesno(
            APP_TITLE,
            "恢复出厂设置会重置所有配置、计数器和 SoC 状态，并立即写入 NVM。\n\n确定继续？",
            icon="warning",
        ):
            return
        self.worker.commands.put({"name": "reset"})
        self.status_var.set("正在恢复出厂设置…")

    # -- 动作 -------------------------------------------------------------
    def toggle_beeper(self) -> None:
        if not self.connected:
            return
        self.worker.commands.put({"name": "beep"})

    def run_test(self, duration: str) -> None:
        if not self.connected:
            return
        text = "10 秒" if duration == "10" else ("60 秒" if duration == "60" else "直到低电量")
        if not messagebox.askyesno(
            APP_TITLE,
            f"开始电池自检（{text}）？\n\n自检需要 AC_CUT 硬件，期间设备会切换到电池运行。",
            icon="warning",
        ):
            return
        self.worker.commands.put({"name": "test", "duration": duration})

    def plan_shutdown(self) -> None:
        if not self.connected:
            return
        try:
            seconds = int(float(self.delay_var.get()))
            restore = int(float(self.restore_var.get()))
        except ValueError:
            messagebox.showerror(APP_TITLE, "延迟和恢复时间必须是整数秒。")
            return
        if seconds < 0 or seconds > 86400:
            messagebox.showerror(APP_TITLE, "延迟必须在 0..86400 秒之间。")
            return
        if restore < 0 or restore > 604800:
            messagebox.showerror(APP_TITLE, "恢复时间必须在 0..604800 秒之间。")
            return

        detail = f"将在 {seconds} 秒后切断 UPS 输出。"
        detail += f"\n将在 {restore} 秒后自动恢复。" if restore else "\n不会自动恢复，需要手动取消或按键恢复。"
        if seconds == 0:
            detail += "\n\n注意：如果本机由 UPS 供电，本机会立即掉电！"
        if not messagebox.askyesno(APP_TITLE, detail + "\n\n确定执行？", icon="warning"):
            return
        self.worker.commands.put(
            {"name": "shutdown", "seconds": seconds, "restore": restore}
        )
        self.status_var.set("正在下发计划…")

    def cancel_shutdown(self) -> None:
        if not self.connected:
            return
        self.worker.commands.put({"name": "cancel"})

    # -- 日志/CSV ---------------------------------------------------------
    def _log(self, message: str, level: str = "info") -> None:
        if level == "proto" and not self.proto_var.get():
            return
        stamp = dt.datetime.now().strftime("%H:%M:%S")
        self.log_text.configure(state="normal")
        self.log_text.insert("end", f"[{stamp}] {message}\n", level)
        lines = int(self.log_text.index("end-1c").split(".")[0])
        if lines > 2500:
            self.log_text.delete("1.0", "600.0")
        self.log_text.configure(state="disabled")
        if self.autoscroll_var.get():
            self.log_text.see("end")

    def clear_log(self) -> None:
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

    def export_log(self) -> None:
        path = filedialog.asksaveasfilename(
            parent=self.root,
            title="导出日志",
            defaultextension=".log",
            filetypes=[("日志文件", "*.log"), ("所有文件", "*.*")],
            initialfile=f"ups_{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}.log",
        )
        if not path:
            return
        try:
            content = self.log_text.get("1.0", "end-1c")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(content + "\n")
            self._log(f"日志已导出到 {path}", "ok")
        except OSError as exc:
            messagebox.showerror(APP_TITLE, f"导出失败：{exc}")

    def choose_csv(self) -> None:
        path = filedialog.asksaveasfilename(
            parent=self.root,
            title="选择 CSV 记录文件",
            defaultextension=".csv",
            filetypes=[("CSV 文件", "*.csv"), ("所有文件", "*.*")],
            initialfile=f"ups_{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}.csv",
        )
        if not path:
            return
        self._close_csv()
        self.csv_path = path
        self.csv_label_var.set(os.path.basename(path))
        self.csv_var.set(True)
        self._open_csv()

    def toggle_csv(self) -> None:
        if self.csv_var.get():
            if not self.csv_path:
                path = filedialog.asksaveasfilename(
                    parent=self.root,
                    title="选择 CSV 记录文件",
                    defaultextension=".csv",
                    filetypes=[("CSV 文件", "*.csv"), ("所有文件", "*.*")],
                    initialfile=f"ups_{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}.csv",
                )
                if not path:
                    self.csv_var.set(False)
                    return
                self.csv_path = path
                self.csv_label_var.set(os.path.basename(path))
            self._open_csv()
        else:
            self._close_csv()
            self.csv_label_var.set("未记录")

    def _open_csv(self) -> None:
        self._close_csv()
        try:
            self.csv_file = open(self.csv_path, "w", newline="", encoding="utf-8-sig")
            self.csv_writer = csv.writer(self.csv_file)
            self.csv_writer.writerow([
                "timestamp", "state", "ac", "output", "charging", "charge_done",
                "soc_pct", "vbat_v", "vin_v", "vout_v", "ibat_a", "iout_a",
                "load_pct", "temp_c", "runtime_s", "faults", "warnings",
                "config_dirty",
            ])
            self.csv_file.flush()
            self._log(f"开始记录 CSV：{self.csv_path}", "ok")
            self.status_var.set(f"正在记录 {os.path.basename(self.csv_path)}")
        except OSError as exc:
            self.csv_writer = None
            self.csv_file = None
            self.csv_var.set(False)
            messagebox.showerror(APP_TITLE, f"无法创建 CSV：{exc}")

    def _close_csv(self) -> None:
        if self.csv_file is not None:
            try:
                self.csv_file.close()
            except Exception:
                pass
        self.csv_file = None
        self.csv_writer = None

    def _write_csv(self, status: dict, soc: float, when: dt.datetime) -> None:
        if self.csv_writer is None:
            return
        temp = status.get("temp_c_x10")
        try:
            temp_text = "" if int(temp) == 9999 else f"{int(temp) / 10.0:.1f}"
        except (TypeError, ValueError):
            temp_text = ""
        try:
            self.csv_writer.writerow([
                when.strftime("%Y-%m-%d %H:%M:%S"),
                status.get("state", ""),
                int(bool(status.get("ac"))),
                int(bool(status.get("output"))),
                int(bool(status.get("charging"))),
                int(bool(status.get("charge_done"))),
                f"{soc:.1f}",
                f"{self._int(status.get('vbat_mv')) / 1000.0:.2f}",
                f"{self._int(status.get('vin_mv')) / 1000.0:.2f}",
                f"{self._int(status.get('vout_mv')) / 1000.0:.2f}",
                f"{self._int(status.get('ibat_ma')) / 1000.0:.3f}",
                f"{self._int(status.get('iout_ma')) / 1000.0:.3f}",
                self._int(status.get("load_pct")),
                temp_text,
                self._int(status.get("runtime_s")),
                self._int(status.get("faults")),
                self._int(status.get("warnings")),
                int(bool(status.get("config_dirty"))),
            ])
            self.csv_file.flush()
        except Exception as exc:
            self._log(f"写 CSV 失败：{exc}", "error")
            self._close_csv()
            self.csv_var.set(False)

    # -- 生命周期 ---------------------------------------------------------
    def _start_smoke_test(self, seconds: float) -> None:
        port = getattr(self.args, "port", None)
        if port:
            self.port_var.set(port)
        if not self.target_port:
            self.toggle_connection()

        def finish() -> None:
            ok = bool(self.last_status) and self.status_count >= 2
            summary = {
                "ok": ok,
                "port": self.session_port,
                "samples": self.status_count,
                "state": (self.last_status or {}).get("state"),
                "soc": (self.last_status or {}).get("soc_x10", 0) / 10.0,
                "fw": self.identity.get("fw"),
                "chart_points": len(self.chart_voltage.samples),
                "config_keys": len(self.config_loaded),
                "geometry": self.root.winfo_geometry(),
                "screen": f"{self.root.winfo_screenwidth()}x"
                          f"{self.root.winfo_screenheight()}",
                "scaling": self.root.tk.call("tk", "scaling"),
            }
            try:
                print("SMOKE " + json.dumps(summary, ensure_ascii=False), flush=True)
            except Exception:
                pass  # --windowed 打包后没有 stdout
            self.exit_code = 0 if ok else 1
            self.on_close(force=True)
        self.root.after(int(max(1.0, seconds) * 1000), finish)

    def on_close(self, force: bool = False) -> None:
        if self._closing:
            return
        if not force and self.csv_writer is not None:
            if not messagebox.askokcancel(APP_TITLE, "正在记录 CSV，仍要退出吗？"):
                return
        self._closing = True
        try:
            self.settings.update({
                "port": self.session_port or self.current_port(),
                "interval": self._interval_value(),
                "auto_reconnect": bool(self.auto_reconnect_var.get()),
                "ring": bool(self.ring_var.get()),
                "show_proto": bool(self.proto_var.get()),
                "geometry": self.root.geometry(),
            })
            save_settings(self.settings)
        except Exception:
            pass
        self._close_csv()
        self.worker.set_target(None)
        self.worker.stop()
        try:
            self.root.destroy()
        except Exception:
            pass

    def _shutdown_worker(self) -> None:
        try:
            self.worker.stop()
            self.worker.join(timeout=1.5)
        except Exception:
            pass
        self._close_csv()

    def run(self) -> int:
        try:
            self.root.mainloop()
        finally:
            self._shutdown_worker()
        return self.exit_code

    @staticmethod
    def _int(value: Any) -> int:
        try:
            return int(value)
        except (TypeError, ValueError):
            return 0


# --------------------------------------------------------------------------
# 入口
# --------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="STM32F103 DC UPS Windows 上位机")
    parser.add_argument("--port", help="启动后连接/预选的串口，例如 COM23")
    parser.add_argument("--tab", choices=["monitor", "config", "actions", "log"],
                        help="启动时选中的页面")
    parser.add_argument("--smoke-test", action="store_true",
                        help="自动连接并采集若干秒后退出（测试用）")
    parser.add_argument("--smoke-seconds", type=float, default=8.0,
                        help="--smoke-test 的采集时长，默认 8 秒")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)

    if IMPORT_ERROR is not None:
        enable_dpi_awareness()
        root = tk.Tk()
        root.withdraw()
        messagebox.showerror(
            APP_TITLE,
            "缺少依赖，无法启动：\n\n"
            f"{IMPORT_ERROR}\n\n"
            "请先执行：\n    pip install pyserial\n\n"
            "并确认 ups_monitor.py 与 ups_gui.py 在同一目录。",
        )
        root.destroy()
        return 2

    enable_dpi_awareness()
    app = UpsApp(args)
    return app.run()


if __name__ == "__main__":
    raise SystemExit(main())
