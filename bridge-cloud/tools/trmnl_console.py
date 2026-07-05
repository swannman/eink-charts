#!/usr/bin/env python3
"""Drive the TRMNL X dev serial console: capture what the panel is actually
showing and step through the slideshow.

The firmware (SERIAL_CONSOLE=1) drops into an interactive console after each
render. This tool opens the USB-CDC port without toggling DTR (so it doesn't
reset the board), spams the dump command until it catches the console window,
then walks the dashboards, saving each framebuffer as a PNG.

Usage:
    python tools/trmnl_console.py capture OUTDIR [N] [--hi]   # grab N screens
    python tools/trmnl_console.py shot OUT.png [--hi]         # one screen
    python tools/trmnl_console.py next                        # advance once

--hi dumps at 2x downscale (936x702) instead of 3x (624x468).
Port defaults to /dev/cu.usbmodem101 (override with TRMNL_PORT).
"""
from __future__ import annotations

import base64
import os
import sys
import time
from pathlib import Path

import serial
from PIL import Image

PORT = os.environ.get("TRMNL_PORT", "/dev/cu.usbmodem101")


def open_port() -> serial.Serial:
    s = serial.Serial()
    s.port = PORT
    s.baudrate = 115200
    s.dtr = False       # don't reset the board on open
    s.rtscts = False
    s.timeout = 0.2
    s.open()
    return s


def _parse_fb(buf: bytes, out_path: Path) -> bool:
    start = buf.find(b"<<<FB ")
    if start < 0:
        return False
    hdr_end = buf.find(b">>>", start)
    w, h = (int(x) for x in buf[start + 6:hdr_end].split())
    b64_start = buf.find(b"\n", hdr_end) + 1
    b64_end = buf.find(b"<<<ENDFB>>>", b64_start)
    if b64_end < 0:
        return False
    b64 = bytes(buf[b64_start:b64_end]).replace(b"\n", b"").replace(b"\r", b"")
    raw = base64.b64decode(b64, validate=False)
    if len(raw) < w * h:
        print(f"  short image: {len(raw)} < {w*h}")
        return False
    Image.frombytes("L", (w, h), raw[:w * h]).save(out_path)
    print(f"  saved {out_path} ({w}x{h})")
    return True


def dump(s: serial.Serial, out_path: Path, hi: bool, grab: bool, timeout: float = 80) -> bool:
    """Capture one framebuffer. If grab, spam the command until the console
    responds (to catch a fresh wake); otherwise assume we're already in it."""
    cmd = b"D" if hi else b"d"
    buf = bytearray()
    started = False
    last_send = 0.0
    deadline = time.time() + timeout
    while time.time() < deadline:
        now = time.time()
        if not started and (grab or last_send == 0) and now - last_send > 1.2:
            s.write(cmd)
            last_send = now
        chunk = s.read(32768)
        if not chunk:
            continue
        buf += chunk
        if b"<<<FB " in buf:
            started = True
        if started and b"<<<ENDFB>>>" in buf:
            return _parse_fb(buf, out_path)
    print("  timed out waiting for framebuffer")
    return False


def advance(s: serial.Serial, grab: bool, timeout: float = 80) -> bool:
    """Advance one dashboard, waiting for the idx= ack."""
    last_send = 0.0
    deadline = time.time() + timeout
    buf = bytearray()
    while time.time() < deadline:
        now = time.time()
        if grab and now - last_send > 1.2:
            s.write(b"n")
            last_send = now
        elif not grab and last_send == 0:
            s.write(b"n")
            last_send = now
        chunk = s.read(4096)
        if chunk:
            buf += chunk
            if b"idx=" in buf:
                return True
    return False


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    hi = "--hi" in sys.argv
    s = open_port()
    time.sleep(0.3)
    try:
        if cmd == "shot":
            out = Path(sys.argv[2])
            return 0 if dump(s, out, hi, grab=True) else 1
        if cmd == "next":
            ok = advance(s, grab=True)
            print("advanced" if ok else "no response")
            return 0 if ok else 1
        if cmd == "capture":
            outdir = Path(sys.argv[2])
            outdir.mkdir(parents=True, exist_ok=True)
            n = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 3
            # First capture grabs the console; the rest reuse the hot session.
            ok0 = dump(s, outdir / "screen_0.png", hi, grab=True)
            if not ok0:
                print("could not reach the console (is the board awake on USB?)")
                return 1
            for i in range(1, n):
                if not advance(s, grab=False):
                    print(f"  advance to {i} failed")
                    break
                time.sleep(0.3)
                dump(s, outdir / f"screen_{i}.png", hi, grab=False)
            s.write(b"s")  # let it sleep
            return 0
        print(f"unknown command: {cmd}")
        return 2
    finally:
        s.close()


if __name__ == "__main__":
    sys.exit(main())
