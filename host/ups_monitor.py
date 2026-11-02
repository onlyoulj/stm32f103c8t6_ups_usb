#!/usr/bin/env python3
"""
Monitor and configure the STM32F103 DC UPS over its USB CDC (Type-C) port.

Requires: pyserial

Examples:
    python ups_monitor.py --port COM7
    python ups_monitor.py --watch
    python ups_monitor.py --set low_pct=25 --save
    python ups_monitor.py --shutdown-after 60 --restore-after 300
    python ups_monitor.py --json
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import sys
import time
from typing import Dict, Optional

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    raise SystemExit(2) from exc


USB_VID = 0x0483
USB_PID = 0x5740

STATE_NAMES = {
    "boot": "BOOT",
    "online": "ONLINE",
    "battery": "ON BATTERY",
    "low_battery": "LOW BATTERY",
    "shutdown": "SHUTDOWN",
    "fault": "FAULT",
}

FAULT_NAMES = {
    1 << 0: "battery_undervoltage",
    1 << 1: "battery_overvoltage",
    1 << 2: "battery_missing",
    1 << 3: "overcurrent",
    1 << 4: "overtemperature",
    1 << 5: "output_undervoltage",
    1 << 6: "output_overvoltage",
    1 << 7: "input_overvoltage",
    1 << 8: "sensor_fault",
    1 << 9: "charger_fault",
    1 << 10: "internal_fault",
}

WARNING_NAMES = {
    1 << 0: "battery_overvoltage",
    1 << 1: "temperature_blocks_charge",
    1 << 2: "temperature_derating",
    1 << 3: "charger_not_reporting",
    1 << 4: "current_sensor_unavailable",
    1 << 5: "temperature_sensor_unavailable",
    1 << 6: "output_sensor_unavailable",
    1 << 7: "config_not_saved",
    1 << 8: "battery_missing",
    1 << 9: "input_low",
    1 << 10: "charge_overcurrent",
    1 << 11: "battery_sensor_unavailable",
    1 << 12: "nvm_write_failed",
}


def bit_names(mask: int, table: Dict[int, str]) -> str:
    if not mask:
        return "none"
    return ", ".join(name for bit, name in table.items() if mask & bit)


def find_port(explicit: Optional[str]) -> str:
    if explicit:
        return explicit

    ports = list(list_ports.comports())
    matching = [p.device for p in ports if p.vid == USB_VID and p.pid == USB_PID]
    if len(matching) == 1:
        return matching[0]
    if len(matching) > 1:
        raise RuntimeError(
            "multiple STM32 CDC ports found: " + ", ".join(matching) +
            " (use --port)"
        )

    resembling = [
        p.device for p in ports
        if p.description and ("STM32" in p.description or "USB Serial" in p.description)
    ]
    if len(resembling) == 1:
        return resembling[0]

    available = ", ".join(p.device for p in ports) or "none"
    raise RuntimeError(
        "no STM32 CDC port found; available ports: " + available + " (use --port)"
    )


class UpsClient:
    def __init__(self, port: str, timeout: float = 1.2) -> None:
        self.port = port
        self.ser = serial.Serial(
            port=port,
            baudrate=115200,
            timeout=timeout,
            write_timeout=1.0,
        )
        time.sleep(0.15)
        self.ser.reset_input_buffer()

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    def command(self, command: str, timeout: float = 1.2) -> dict:
        self.ser.timeout = timeout
        self.ser.reset_input_buffer()
        self.ser.write((command + "\r\n").encode("ascii"))
        self.ser.flush()
        raw = self.ser.readline()
        if not raw:
            raise TimeoutError(f"no response to {command!r}")
        text = raw.decode("utf-8", errors="replace").strip()
        try:
            response = json.loads(text)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"invalid response to {command!r}: {text!r}") from exc
        if not response.get("ok"):
            raise RuntimeError(response.get("error", "device error"))
        return response

    def status(self) -> dict:
        return self.command("XSTATUS")

    def config(self) -> dict:
        return self.command("XCONFIG")

    def identify(self) -> dict:
        return self.command("XID")

    def set_value(self, key: str, value: str) -> dict:
        return self.command(f"XSET {key}={value}")

    def save(self) -> dict:
        return self.command("XSAVE")

    def reset(self) -> dict:
        return self.command("XRESET", timeout=2.5)

    def shutdown_after(self, seconds: int, restore_seconds: int = 0) -> dict:
        if restore_seconds:
            return self.command(f"XSHUTDOWN {seconds} {restore_seconds}")
        return self.command(f"XSHUTDOWN {seconds}")

    def cancel_shutdown(self) -> dict:
        return self.command("XCANCEL")

    def test(self, duration: str) -> dict:
        return self.command(f"XTEST {duration}", timeout=2.0)

    def beep(self) -> dict:
        return self.command("XBEEP")


def format_runtime(seconds: int) -> str:
    if seconds is None or seconds <= 0:
        return "--"
    hours, rem = divmod(int(seconds), 3600)
    minutes, secs = divmod(rem, 60)
    if hours:
        return f"{hours}h {minutes:02d}m"
    if minutes:
        return f"{minutes}m {secs:02d}s"
    return f"{secs}s"


def format_current(ma: int) -> str:
    amps = ma / 1000.0
    state = "charge" if amps > 0.05 else ("load" if amps < -0.05 else "idle")
    return f"{amps:+.2f} A ({state})"


def human_status(st: dict) -> str:
    on_off = lambda value: "on" if value else "off"
    soc = st.get("soc_x10", 0) / 10.0
    temp = st.get("temp_c_x10")
    temp_text = "--" if temp is None or temp == 9999 else f"{temp / 10.0:.1f} C"

    lines = []
    lines.append(
        f"{STATE_NAMES.get(st.get('state'), st.get('state', '?'))}  "
        f"AC {on_off(st.get('ac'))}  output {on_off(st.get('output'))}  "
        f"battery {soc:.1f}% ({st.get('vbat_mv', 0) / 1000.0:.2f} V)"
    )
    lines.append(
        f"input {st.get('vin_mv', 0) / 1000.0:.2f} V  "
        f"output {st.get('vout_mv', 0) / 1000.0:.2f} V  "
        f"battery current {format_current(st.get('ibat_ma', 0))}"
    )
    lines.append(
        f"load {st.get('load_pct', 0)}%  temperature {temp_text}  "
        f"runtime {format_runtime(st.get('runtime_s', 0))}  "
        f"AC failures {st.get('ac_fail_count', 0)}"
    )
    lines.append(
        f"charging {on_off(st.get('charging'))}  "
        f"charge done {on_off(st.get('charge_done'))}  "
        f"beeper {on_off(st.get('beeper'))}  "
        f"config saved {'no' if st.get('config_dirty') else 'yes'}"
    )
    if st.get("shutdown_pending"):
        lines.append(
            f"shutdown pending: {format_runtime(st.get('shutdown_remaining_s', 0))}"
        )
    if st.get("restore_remaining_s"):
        lines.append(f"restore in {format_runtime(st.get('restore_remaining_s', 0))}")
    lines.append(f"faults: {bit_names(st.get('faults', 0), FAULT_NAMES)}")
    lines.append(f"warnings: {bit_names(st.get('warnings', 0), WARNING_NAMES)}")
    if not st.get("ina_ok"):
        lines.append("note: battery current sensor is not responding")
    if not st.get("ntc_ok") and st.get("state") != "online":
        lines.append("note: battery temperature sensor is not responding")
    return "\n".join(lines)


def confirm(prompt: str) -> bool:
    if not sys.stdin.isatty():
        print("refusing dangerous action without --yes (stdin is not a TTY)",
              file=sys.stderr)
        return False
    try:
        return input(prompt + " [y/N] ").strip().lower() in ("y", "yes")
    except EOFError:
        return False


def make_client(args: argparse.Namespace) -> UpsClient:
    return UpsClient(find_port(args.port))


def print_response(args: argparse.Namespace, response: dict) -> None:
    if args.json:
        print(json.dumps(response, ensure_ascii=False))
    else:
        print(json.dumps(response, ensure_ascii=False, indent=2))


def run_watch(args: argparse.Namespace) -> int:
    while True:
        try:
            client = make_client(args)
            try:
                while True:
                    st = client.status()
                    if args.json:
                        print(json.dumps(st, ensure_ascii=False), flush=True)
                    else:
                        stamp = _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                        print(f"[{stamp}]")
                        print(human_status(st), flush=True)
                        print()
                    time.sleep(max(1, args.interval))
            finally:
                client.close()
        except KeyboardInterrupt:
            return 0
        except Exception as exc:
            print(f"monitor: {exc}; reconnecting...", file=sys.stderr, flush=True)
            time.sleep(2)


def run_once(args: argparse.Namespace) -> int:
    client = make_client(args)
    try:
        if args.id:
            print_response(args, client.identify())
            return 0

        if args.set:
            for item in args.set:
                if "=" not in item:
                    print(f"invalid --set item {item!r}; expected key=value", file=sys.stderr)
                    return 2
                key, value = item.split("=", 1)
                response = client.set_value(key.strip(), value.strip())
                if args.json:
                    print(json.dumps(response, ensure_ascii=False))
                else:
                    print(f"set {key.strip()} = {value.strip()}")
            if args.save:
                response = client.save()
                if args.json:
                    print(json.dumps(response, ensure_ascii=False))
                else:
                    print("configuration saved")

        if args.reset:
            if not args.yes and not confirm("Reset configuration to defaults?"):
                return 1
            print_response(args, client.reset())

        if args.cancel:
            print_response(args, client.cancel_shutdown())

        if args.test is not None:
            print_response(args, client.test(str(args.test)))

        if args.beep:
            print_response(args, client.beep())

        if args.shutdown_after is not None:
            if not args.yes:
                if not confirm(
                    f"Turn UPS output off in {args.shutdown_after} s"
                    + (f" and restore after {args.restore_after} s"
                       if args.restore_after else "") + "?"
                ):
                    return 1
            print_response(
                args,
                client.shutdown_after(args.shutdown_after, args.restore_after),
            )

        # Always finish with a status snapshot unless a dangerous action was
        # requested with zero delay (the output may already be off).
        if not args.json:
            try:
                print()
                print(human_status(client.status()))
            except Exception:
                pass
        return 0
    finally:
        client.close()


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Monitor/config the STM32F103 DC UPS over USB CDC.",
    )
    p.add_argument("--port", help="serial port (auto-detect 0483:5740 by default)")
    p.add_argument("--json", action="store_true", help="print raw JSON responses")
    p.add_argument("--watch", action="store_true", help="poll until interrupted")
    p.add_argument("--interval", type=int, default=5, help="watch interval seconds")
    p.add_argument("--id", action="store_true", help="show device identity")
    p.add_argument("--set", action="append", metavar="KEY=VALUE",
                   help="change a configuration value (repeatable)")
    p.add_argument("--save", action="store_true", help="save configuration after --set")
    p.add_argument("--reset", action="store_true", help="reset configuration to defaults")
    p.add_argument("--shutdown-after", type=int, metavar="SECONDS",
                   help="turn the UPS output off after this many seconds")
    p.add_argument("--restore-after", type=int, default=0, metavar="SECONDS",
                   help="with --shutdown-after: restore output after this many seconds")
    p.add_argument("--cancel", action="store_true", help="cancel a pending shutdown")
    p.add_argument("--test", metavar="SEC|low",
                   help="run a battery-transfer test (requires AC-cut hardware)")
    p.add_argument("--beep", action="store_true", help="toggle the beeper")
    p.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    return p


def main() -> int:
    args = build_parser().parse_args()
    if args.watch:
        return run_watch(args)
    try:
        return run_once(args)
    except KeyboardInterrupt:
        return 0
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
