#!/usr/bin/env python3
"""
Low-battery shutdown guard for the STM32F103 DC UPS.

Runs on the powered host (Raspberry Pi, mini PC, NAS, ...).  It polls the UPS
over USB CDC and executes a shutdown command when the UPS is on battery and the
SoC reaches the configured threshold.  Dry-run by default; pass --execute to
actually run the command.

This is an alternative to NUT.  Do not run both against the same serial port.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import subprocess
import sys
import time
from typing import Optional

from ups_monitor import UpsClient, find_port


def log(message: str) -> None:
    stamp = _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{stamp}] {message}", file=sys.stderr, flush=True)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Shut down the host on UPS low battery.")
    p.add_argument("--port", help="serial port (auto-detect by default)")
    p.add_argument("--threshold", type=int, default=20,
                   help="SoC percentage that triggers the shutdown (default 20)")
    p.add_argument("--interval", type=float, default=5.0,
                   help="poll interval seconds (default 5)")
    p.add_argument("--shutdown-cmd", default="/sbin/shutdown -h now",
                   help="command to run at low battery")
    p.add_argument("--execute", action="store_true",
                   help="actually run the shutdown command (otherwise dry-run)")
    p.add_argument("--once", action="store_true",
                   help="evaluate once and exit (useful for testing)")
    return p.parse_args()


def check_once(client: UpsClient, args: argparse.Namespace) -> Optional[bool]:
    """Return True if the shutdown condition is active, False if not."""
    st = client.status()
    ac = bool(st.get("ac"))
    soc_x10 = int(st.get("soc_x10", 1000))
    low = (
        soc_x10 <= max(0, args.threshold) * 10
        or bool(st.get("low"))
        or bool(st.get("critical"))
        or st.get("state") == "low_battery"
    )
    state = st.get("state", "?")

    if not ac and low:
        log(
            f"LOW BATTERY: state={state} soc={soc_x10 / 10.0:.1f}% "
            f"vbat={st.get('vbat_mv', 0) / 1000.0:.2f}V "
            f"runtime={st.get('runtime_s', 0)}s"
        )
        return True
    return False


def main() -> int:
    args = parse_args()
    if args.threshold < 0 or args.threshold > 100:
        log("threshold must be 0..100")
        return 2

    triggered = False
    while True:
        client: Optional[UpsClient] = None
        try:
            port = find_port(args.port)
            client = UpsClient(port)
            log(f"connected to {port}")

            while True:
                condition = check_once(client, args)
                if condition and not triggered:
                    triggered = True
                    if args.execute:
                        log(f"executing: {args.shutdown_cmd}")
                        subprocess.run(args.shutdown_cmd, shell=True, check=False)
                    else:
                        log(
                            "dry-run: shutdown condition reached; "
                            "rerun with --execute to act"
                        )
                elif not condition and triggered:
                    st = client.status()
                    if st.get("ac"):
                        log("AC restored; guard re-armed")
                        triggered = False
                    else:
                        # The host may still be shutting down; do not re-trigger
                        # repeatedly while the condition remains.
                        pass

                if args.once:
                    return 0
                time.sleep(max(0.5, args.interval))
        except KeyboardInterrupt:
            return 0
        except Exception as exc:
            log(f"guard error: {exc}; retrying in 3 s")
            time.sleep(3)
        finally:
            if client is not None:
                client.close()


if __name__ == "__main__":
    raise SystemExit(main())
