#!/usr/bin/env python3
"""Bounded serial capture that cannot lose the crash.

Logs EVERY line raw to a file and prints only a narrowed view to stdout.
The narrowing is presentation; the raw file is the record. A capture that
filters what it *stores* will one day record a reboot and discard the panic
that preceded it (HARDWARE.md pitfall #28) — this one cannot.

Usage:
    python3 tools/capture.py PORT SECONDS RAWFILE

Run it with the ESP-IDF venv python (it has pyserial). Typical use:

    . ~/esp/esp-idf/export.sh
    PORT=$(ls /dev/cu.usbmodem* | head -1); [ -z "$PORT" ] && exit 1
    python3 tools/capture.py "$PORT" 120 /tmp/run_raw.log > /tmp/run.log

Feed RAWFILE to tools/snap_rx.py to extract any SNAP screenshots in it.
"""
import serial
import sys
import time

KEY = ("render perf", "uptime=", "rst:", "Loaded app", "Guru", "PANIC",
       "abort", "assert", "Backtrace", "CORRUPT", "Stack", "WDT", "watchdog",
       "selftest", "SNAP_BEGIN", "SNAP_END")


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    port, secs, rawpath = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    s = serial.Serial(port, 115200, timeout=1)
    t0 = time.time()
    with open(rawpath, "w") as raw:
        while time.time() - t0 < secs:
            try:
                line = s.readline().decode("utf8", "replace").rstrip()
            except Exception as e:
                print(f"[serial error] {e}", flush=True)
                break
            if not line:
                continue
            stamp = time.time() - t0
            raw.write(f"{stamp:6.1f}  {line}\n")
            raw.flush()
            low = line.lower()
            if any(k.lower() in low for k in KEY):
                print(f"{stamp:6.1f}  {line}", flush=True)


if __name__ == "__main__":
    main()
