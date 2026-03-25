#!/usr/bin/env python3
"""
chirp_ctrl.py - Interactive remote control for the Chirp launcher.

Controls the device launcher menu over USB serial using MSG_INTERNAL_CONTROL
frames.  Also monitors and prints all device log output in real-time.

Key bindings (interactive mode)
--------------------------------
  LEFT  / h / a   -- navigate prev (rotate left)
  RIGHT / l / d   -- navigate next (rotate right)
  ENTER / k / s   -- short click (focus/switch)
  SPACE / x       -- long press  (start/stop script)
  w               -- inject a single Wren expression (prompts inline)
  q / Ctrl-C      -- quit

Single-command mode (non-interactive)
--------------------------------------
  chirp_ctrl.py --cmd left
  chirp_ctrl.py --cmd right
  chirp_ctrl.py --cmd click
  chirp_ctrl.py --cmd long
  chirp_ctrl.py --cmd wren --wren 'Log.info("hello")'

Usage
-----
  python3 tools/chirp_ctrl.py
  python3 tools/chirp_ctrl.py -p /dev/ttyACM0
  python3 tools/chirp_ctrl.py --cmd long          # send one long-press and exit
  python3 tools/chirp_ctrl.py --cmd wren --wren 'Log.info("hi")'
"""

import argparse
import select
import struct
import sys
import termios
import threading
import time
import tty
from typing import Optional

try:
    import serial
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")


# ── Protocol constants (mirror usb_serial_protocol.h) ────────────────────────

FRAME_SYNC0       = 0xAA
FRAME_SYNC1       = 0x55
FRAME_OVERHEAD    = 8
FRAME_MAX_PAYLOAD = 4096

MSG_LOG_TEXT         = 0x01
MSG_INTERNAL_CONTROL = 0x11
MSG_PING             = 0x7E
MSG_PONG             = 0x7F

CTRL_NAV_PREV    = 0x00
CTRL_NAV_NEXT    = 0x01
CTRL_SELECT      = 0x02
CTRL_LONG_PRESS  = 0x03
CTRL_WREN_INJECT = 0x04


# ── CRC16-CCITT ───────────────────────────────────────────────────────────────

def _crc16(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
        crc &= 0xFFFF
    return crc


# ── Frame builder ─────────────────────────────────────────────────────────────

def _build_frame(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    header = struct.pack("<BBBBH", FRAME_SYNC0, FRAME_SYNC1, msg_type, seq, len(payload))
    crc = _crc16(header[2:] + payload)
    return header + payload + struct.pack("<H", crc)


def _ctrl_frame(op: int, seq: int, extra: bytes = b"") -> bytes:
    return _build_frame(MSG_INTERNAL_CONTROL, seq, bytes([op]) + extra)


# ── Serial connection with log monitor thread ────────────────────────────────

class ChirpCtrl:
    def __init__(self, port: str, baud: int = 115200):
        self._port = port
        self._baud = baud
        self._ser: Optional[serial.Serial] = None
        self._seq = 1
        self._monitor_thread: Optional[threading.Thread] = None
        self._running = False

    def connect(self) -> None:
        self._ser = serial.Serial(self._port, self._baud, timeout=0.05)
        self._running = True
        self._monitor_thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self._monitor_thread.start()

    def close(self) -> None:
        self._running = False
        if self._ser and self._ser.is_open:
            self._ser.close()

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.close()

    def _next_seq(self) -> int:
        s = self._seq
        self._seq = (self._seq % 255) + 1
        return s

    # ── Background log monitor ────────────────────────────────────────────────

    def _monitor_loop(self) -> None:
        """Read all incoming bytes; print MSG_LOG_TEXT frames."""
        state = 0
        hdr = bytearray()
        payload = bytearray()
        crc_bytes = bytearray()
        plen = 0

        while self._running:
            try:
                raw = self._ser.read(1)
            except Exception:
                break
            if not raw:
                continue
            b = raw[0]

            if state == 0:
                if b == FRAME_SYNC0:
                    state = 1
            elif state == 1:
                if b == FRAME_SYNC1:
                    hdr.clear()
                    state = 2
                else:
                    state = 0
            elif state == 2:
                hdr.append(b)
                if len(hdr) == 4:
                    plen = hdr[2] | (hdr[3] << 8)
                    if plen > FRAME_MAX_PAYLOAD:
                        state = 0
                        continue
                    payload.clear()
                    state = 3 if plen > 0 else 4
            elif state == 3:
                payload.append(b)
                if len(payload) == plen:
                    crc_bytes.clear()
                    state = 4
            elif state == 4:
                crc_bytes.append(b)
                if len(crc_bytes) == 2:
                    recv_crc = crc_bytes[0] | (crc_bytes[1] << 8)
                    calc_data = bytes(hdr) + bytes(payload)
                    calc_crc = _crc16(calc_data)
                    if recv_crc == calc_crc:
                        ftype = hdr[0]
                        if ftype == MSG_LOG_TEXT:
                            text = bytes(payload).decode("utf-8", errors="replace").rstrip()
                            print(text, flush=True)
                    state = 0

    # ── Send helpers ──────────────────────────────────────────────────────────

    def send_nav_prev(self) -> None:
        self._ser.write(_ctrl_frame(CTRL_NAV_PREV, self._next_seq()))

    def send_nav_next(self) -> None:
        self._ser.write(_ctrl_frame(CTRL_NAV_NEXT, self._next_seq()))

    def send_select(self) -> None:
        self._ser.write(_ctrl_frame(CTRL_SELECT, self._next_seq()))

    def send_long_press(self) -> None:
        self._ser.write(_ctrl_frame(CTRL_LONG_PRESS, self._next_seq()))

    def send_wren(self, source: str) -> None:
        src_bytes = source.encode("utf-8")
        self._ser.write(_ctrl_frame(CTRL_WREN_INJECT, self._next_seq(), src_bytes))

    def ping(self) -> bool:
        seq = self._next_seq()
        self._ser.write(_build_frame(MSG_PING, seq))
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            raw = self._ser.read(FRAME_OVERHEAD + 8)
            if MSG_PONG in raw:
                return True
        return False


# ── Interactive terminal UI ───────────────────────────────────────────────────

HELP = """\
  Controls:
    ←/h/a       prev (nav left)
    →/l/d       next (nav right)
    Enter/k/s   short click (focus)
    Space/x     long press  (start/stop)
    w           inject Wren expression
    q / Ctrl+C  quit
"""

ANSI_CLEAR  = "\033[2J\033[H"
ANSI_BOLD   = "\033[1m"
ANSI_RESET  = "\033[0m"
ANSI_GREEN  = "\033[32m"
ANSI_YELLOW = "\033[33m"
ANSI_CYAN   = "\033[36m"


def _read_key_raw() -> str:
    """Read one key (or escape sequence) from stdin without echo."""
    ch = sys.stdin.read(1)
    if ch == "\x1b":
        # Check for arrow keys [ESC [ A/B/C/D]
        if select.select([sys.stdin], [], [], 0.05)[0]:
            ch2 = sys.stdin.read(1)
            if ch2 == "[" and select.select([sys.stdin], [], [], 0.05)[0]:
                ch3 = sys.stdin.read(1)
                return f"\x1b[{ch3}"
    return ch


def interactive(ctrl: ChirpCtrl) -> None:
    print(f"{ANSI_BOLD}Chirp Remote Control{ANSI_RESET}")
    print(HELP)
    print("Device logs will appear below. Use keys to control launcher.\n")

    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        while True:
            # Non-blocking key check (100ms poll so logs keep printing)
            if not select.select([sys.stdin], [], [], 0.1)[0]:
                continue
            key = _read_key_raw()

            if key in ("q", "\x03", "\x04"):   # q / Ctrl-C / Ctrl-D
                break
            elif key in ("\x1b[D", "h", "a"):  # left arrow, h, a
                ctrl.send_nav_prev()
                sys.stderr.write(f"{ANSI_CYAN}→ nav prev{ANSI_RESET}\n")
                sys.stderr.flush()
            elif key in ("\x1b[C", "l", "d"):  # right arrow, l, d
                ctrl.send_nav_next()
                sys.stderr.write(f"{ANSI_CYAN}→ nav next{ANSI_RESET}\n")
                sys.stderr.flush()
            elif key in ("\r", "\n", "k", "s"):
                ctrl.send_select()
                sys.stderr.write(f"{ANSI_GREEN}→ short click{ANSI_RESET}\n")
                sys.stderr.flush()
            elif key in (" ", "x"):
                ctrl.send_long_press()
                sys.stderr.write(f"{ANSI_YELLOW}→ long press{ANSI_RESET}\n")
                sys.stderr.flush()
            elif key == "w":
                # Temporarily restore canonical mode to type Wren source
                termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
                sys.stderr.write("Wren> ")
                sys.stderr.flush()
                src = sys.stdin.readline().strip()
                tty.setraw(fd)
                if src:
                    ctrl.send_wren(src)
                    sys.stderr.write(f"{ANSI_CYAN}→ wren: {src}{ANSI_RESET}\n")
                    sys.stderr.flush()
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
        print("\nBye.")


# ── CLI ───────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Interactive or single-shot Chirp launcher remote control",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("-p", "--port", default="/dev/ttyACM0", help="Serial port")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument(
        "--cmd",
        choices=["left", "right", "click", "long", "wren", "ping"],
        help="Send a single command and exit (non-interactive)",
    )
    parser.add_argument(
        "--wren",
        metavar="SOURCE",
        help="Wren source to inject (used with --cmd wren)",
    )
    parser.add_argument(
        "--script",
        metavar="COMMANDS",
        help="Run a scripted sequence like 'right,right,long,wait:1.5,long' and exit",
    )
    args = parser.parse_args()

    with ChirpCtrl(args.port, args.baud) as ctrl:
        time.sleep(0.2)   # let monitor thread drain any pending output

        if args.script:
            for token in args.script.split(","):
                step = token.strip()
                if not step:
                    continue
                if step.startswith("wait:"):
                    time.sleep(float(step.split(":", 1)[1]))
                    continue
                if step == "left":
                    ctrl.send_nav_prev()
                    print("sent: nav prev")
                elif step == "right":
                    ctrl.send_nav_next()
                    print("sent: nav next")
                elif step == "click":
                    ctrl.send_select()
                    print("sent: short click")
                elif step == "long":
                    ctrl.send_long_press()
                    print("sent: long press")
                elif step == "ping":
                    ok = ctrl.ping()
                    print("pong: OK" if ok else "pong: timeout")
                else:
                    parser.error(f"Unknown script step: {step}")
                time.sleep(0.5)
            time.sleep(1.0)
        elif args.cmd:
            if args.cmd == "left":
                ctrl.send_nav_prev()
                print("sent: nav prev")
            elif args.cmd == "right":
                ctrl.send_nav_next()
                print("sent: nav next")
            elif args.cmd == "click":
                ctrl.send_select()
                print("sent: short click")
            elif args.cmd == "long":
                ctrl.send_long_press()
                print("sent: long press")
            elif args.cmd == "wren":
                if not args.wren:
                    parser.error("--wren SOURCE required with --cmd wren")
                ctrl.send_wren(args.wren)
                print(f"sent: wren inject: {args.wren}")
            elif args.cmd == "ping":
                ok = ctrl.ping()
                print("pong: OK" if ok else "pong: timeout")
            time.sleep(0.5)   # let device respond and logs print
        else:
            interactive(ctrl)


if __name__ == "__main__":
    main()
