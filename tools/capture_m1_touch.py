"""Capture live M1 touch diagnostics from the CH343 debug UART.

Usage: python capture_m1_touch.py COM8 90
"""

import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
duration_seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0
deadline = time.monotonic() + duration_seconds

with serial.Serial(port, 115200, timeout=0.25) as connection:
    print(f"M1_TOUCH_CAPTURE port={port} duration_s={duration_seconds:g}", flush=True)
    while time.monotonic() < deadline:
        raw_line = connection.readline()
        if raw_line:
            print(raw_line.decode("utf-8", "replace").rstrip(), flush=True)
