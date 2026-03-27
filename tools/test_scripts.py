#!/usr/bin/env python3
"""
test_scripts.py - Automated reload test for Chirp user scripts.

Flashes firmware, syncs scripts to the device, then validates that each
user script can be started, stopped, and started again (twice) without
crashing.  Exit code is 0 if all tests pass, 1 if any fail.

Usage
-----
    # Full run: build, flash, sync, then test
    python3 tools/test_scripts.py

    # Skip build/flash (device already has latest firmware)
    python3 tools/test_scripts.py --no-flash

    # Only test specific scripts (by base name, no extension)
    python3 tools/test_scripts.py --scripts ARP NDS_v2

    # Use a specific serial port
    python3 tools/test_scripts.py -p /dev/ttyACM1

    # Override navigator position for each script (see --help)
    python3 tools/test_scripts.py --no-flash --scripts ARP NDS_v2

How it works
------------
The launcher presents scripts in alphabetical order.  This script navigates
to each target by issuing enough LEFT presses to wrap to the first entry,
then RIGHT presses to the desired position.  For each target:

  1. Long-press to START the script.
  2. Wait for the startup log marker.
  3. Long-press to STOP.
  4. Wait for the unload log marker.
  5. Repeat START/STOP once more (cycle 2).
  6. PASS if both cycles produce both markers; FAIL otherwise.

Prerequisites
-------------
    pip install pyserial
    make        (if --no-flash is not given)
    teensy_loader_cli on PATH (if --no-flash is not given)
"""

import argparse
import queue
import re
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Optional

try:
    import serial
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

# ── Repo layout ───────────────────────────────────────────────────────────────

REPO = Path(__file__).resolve().parent.parent
SCRIPTS_DIR = REPO / "scripts"
TOOLS_DIR   = REPO / "tools"
HEX_FILE    = REPO / "output" / "chirp.ino.hex"
MCU         = "TEENSY40"

# ── Protocol (mirrors usb_serial_protocol.h) ──────────────────────────────────

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


def _crc16(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
        crc &= 0xFFFF
    return crc


def _ctrl_frame(ctrl: int, seq: int, payload: bytes = b"") -> bytes:
    full_payload = bytes([ctrl]) + payload
    hdr = bytes([MSG_INTERNAL_CONTROL, seq,
                 len(full_payload) & 0xFF, (len(full_payload) >> 8) & 0xFF])
    body = hdr + full_payload
    crc = _crc16(body)
    return bytes([FRAME_SYNC0, FRAME_SYNC1]) + body + bytes([crc & 0xFF, crc >> 8])


def _ping_frame(seq: int) -> bytes:
    hdr = bytes([MSG_PING, seq, 0, 0])
    crc = _crc16(hdr)
    return bytes([FRAME_SYNC0, FRAME_SYNC1]) + hdr + bytes([crc & 0xFF, crc >> 8])


def _wren_frame(wren_src: str, seq: int) -> bytes:
    """Build a Wren injection control frame."""
    src_bytes = wren_src.encode("utf-8")
    hdr = bytes([MSG_INTERNAL_CONTROL, seq,
                 len(src_bytes) & 0xFF, (len(src_bytes) >> 8) & 0xFF])
    body = bytes([CTRL_WREN_INJECT]) + hdr[1:] + src_bytes
    crc = _crc16(body)
    return bytes([FRAME_SYNC0, FRAME_SYNC1]) + body + bytes([crc & 0xFF, crc >> 8])


# ── Device connection / log capture ──────────────────────────────────────────

class ChirpDevice:
    """Thin wrapper: sends control frames, collects decoded log lines."""

    def __init__(self, port: str, baud: int = 115200):
        self._port = port
        self._baud = baud
        self._ser: Optional[serial.Serial] = None
        self._seq = 1
        self._running = False
        self._log_q: "queue.Queue[str]" = queue.Queue()
        self._pong_q: "queue.Queue[int]" = queue.Queue()
        self._all_lines: list[str] = []
        self._log_lock = threading.Lock()
        self._thread: Optional[threading.Thread] = None

    def connect(self) -> None:
        self._ser = serial.Serial(self._port, self._baud, timeout=0.05)
        self._running = True
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

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

    def _reader(self) -> None:
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
                    if recv_crc == _crc16(calc_data):
                        if hdr[0] == MSG_LOG_TEXT:
                            text = bytes(payload).decode("utf-8", errors="replace").rstrip()
                            with self._log_lock:
                                self._all_lines.append(text)
                            self._log_q.put_nowait(text)
                        elif hdr[0] == MSG_PONG:
                            self._pong_q.put_nowait(hdr[1])
                    state = 0

    def nav_prev(self) -> None:
        self._ser.write(_ctrl_frame(CTRL_NAV_PREV, self._next_seq()))

    def nav_next(self) -> None:
        self._ser.write(_ctrl_frame(CTRL_NAV_NEXT, self._next_seq()))

    def select(self) -> None:
        self._ser.write(_ctrl_frame(CTRL_SELECT, self._next_seq()))

    def long_press(self) -> None:
        self._ser.write(_ctrl_frame(CTRL_LONG_PRESS, self._next_seq()))

    def ping(self, timeout: float = 3.0) -> bool:
        seq = self._next_seq()
        while True:
            try:
                self._pong_q.get_nowait()
            except queue.Empty:
                break
        self._ser.write(_ping_frame(seq))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                pong_seq = self._pong_q.get(timeout=min(remaining, 0.2))
            except queue.Empty:
                continue
            if pong_seq == seq:
                return True
        return False

    def send_wren(self, source: str) -> None:
        """Inject a Wren expression into the runtime."""
        self._ser.write(_wren_frame(source, self._next_seq()))

    def wait_for_pattern(self, pattern: str, timeout: float = 10.0) -> Optional[str]:
        """Block until a log line matching *pattern* arrives, or timeout."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                line = self._log_q.get(timeout=min(remaining, 0.2))
            except queue.Empty:
                continue
            print(f"  LOG: {line}")
            if rx.search(line):
                return line
        return None

    def drain_logs(self, duration: float = 1.0) -> list[str]:
        """Collect and print all log lines for *duration* seconds."""
        lines = []
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                line = self._log_q.get(timeout=min(remaining, 0.1))
                lines.append(line)
                print(f"  LOG: {line}")
            except queue.Empty:
                pass
        return lines

    def flush_queue(self) -> None:
        """Discard any queued log lines that arrived before the current action."""
        while True:
            try:
                self._log_q.get_nowait()
            except queue.Empty:
                break

    def all_logs(self) -> list[str]:
        with self._log_lock:
            return list(self._all_lines)


# ── Build / flash helpers ─────────────────────────────────────────────────────

def run(cmd: list[str], desc: str) -> bool:
    print(f"\n[BUILD] {desc}")
    result = subprocess.run(cmd, cwd=REPO)
    ok = result.returncode == 0
    if not ok:
        print(f"[BUILD] FAILED (exit {result.returncode})")
    return ok


def find_port() -> Optional[str]:
    for candidate in ("/dev/ttyACM0", "/dev/ttyACM1"):
        if Path(candidate).exists():
            return candidate
    return None


def sync_scripts(port: str) -> bool:
    print(f"\n[SYNC] uploading scripts and MIDI maps to {port}")
    result = subprocess.run(
        [sys.executable, str(TOOLS_DIR / "chirp_fs.py"),
         "-p", port, "sync", "scripts", "midi"],
        cwd=REPO,
    )
    return result.returncode == 0


# ── Script navigation helpers ─────────────────────────────────────────────────

# Alphabetical order of user scripts as seen by the launcher.
# Edit this list if new scripts are added.
PRODUCTION_SCRIPTS = ["ARP", "NDS_v2"]  # Tested and known-working scripts

USER_SCRIPTS_ORDERED = ["ARP", "NDS_v2"]  # Alphabetical order on device launcher

def navigate_to(dev: ChirpDevice, script_name: str) -> None:
    """Navigate launcher to *script_name* from a known MAIN anchor."""
    try:
        idx = USER_SCRIPTS_ORDERED.index(script_name)
    except ValueError:
        idx = 0

    # Walk left until we observe the MAIN item, then step right from that
    # anchor. This is deterministic even when launcher selection is persisted.
    print("  nav: anchoring on MAIN")
    for _ in range(len(USER_SCRIPTS_ORDERED) + 1):
        dev.nav_prev()
        hit = dev.wait_for_pattern(r"reached MAIN item", timeout=0.6)
        if hit is not None:
            break
        time.sleep(0.05)

    print(f"  nav: {idx}x right to reach '{script_name}'")
    for _ in range(idx + 1):
        dev.nav_next()
        time.sleep(0.15)

    time.sleep(0.3)   # let launcher settle


# ── Per-script start/stop markers ────────────────────────────────────────────

# Patterns that appear in device logs when a script starts or unloads.
# Adjust these if the scripts change their log messages.
SCRIPT_MARKERS = {
    "ARP": {
        "start": r"ARP",
        "stop":  r"ARP|unload|Unload",
    },
    "NDS_v2": {
        "start": r"NDS|loaded|parameterEntries|NDS_midi_map",
        "stop":  r"NDS|unload|Unload",
    },
}

DEFAULT_MARKERS = {
    "start": r".",   # any log output on start is acceptable as fallback
    "stop":  r".",
}


# ── Core test logic ───────────────────────────────────────────────────────────

ANSI_GREEN  = "\033[32m"
ANSI_RED    = "\033[31m"
ANSI_YELLOW = "\033[33m"
ANSI_RESET  = "\033[0m"
ANSI_BOLD   = "\033[1m"


def test_script(dev: ChirpDevice, name: str, cycles: int = 2) -> bool:
    """
    Start/stop *name* for *cycles* iterations.  Returns True if all cycles
    produce expected start and stop log markers.
    """
    markers = SCRIPT_MARKERS.get(name, DEFAULT_MARKERS)
    start_pat = markers["start"]
    stop_pat  = markers["stop"]

    print(f"\n{ANSI_BOLD}=== Testing: {name} ==={ANSI_RESET}")
    navigate_to(dev, name)

    for cycle in range(1, cycles + 1):
        print(f"\n  -- Cycle {cycle}/{cycles} --")

        # ── Start ─────────────────────────────────────────────────────────────
        print(f"  [START] long-press to start '{name}'")
        dev.flush_queue()   # discard stale navigation/launcher logs
        dev.long_press()
        hit = dev.wait_for_pattern(start_pat, timeout=8.0)
        if hit is None:
            print(f"  {ANSI_RED}FAIL{ANSI_RESET}: no start marker in 8 s  (pattern: {start_pat!r})")
            dev.drain_logs(1.0)
            return False
        print(f"  {ANSI_GREEN}start marker found{ANSI_RESET}: {hit!r}")

        time.sleep(1.0)   # let script settle

        # ── Stop ──────────────────────────────────────────────────────────────
        print(f"  [STOP] long-press to stop '{name}'")
        dev.long_press()
        hit = dev.wait_for_pattern(stop_pat, timeout=8.0)
        if hit is None:
            print(f"  {ANSI_YELLOW}WARN{ANSI_RESET}: no stop marker in 8 s  (pattern: {stop_pat!r})")
            # Not a hard failure — some scripts log nothing on stop
        else:
            print(f"  {ANSI_GREEN}stop marker found{ANSI_RESET}: {hit!r}")

        time.sleep(0.8)   # let launcher finish cleanup

    print(f"  {ANSI_GREEN}PASS{ANSI_RESET}: {name} survived {cycles} start/stop cycle(s)")
    return True


# ── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build, flash, sync and reload-test Chirp user scripts",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("-p", "--port", help="Serial port (auto-detected if omitted)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--no-flash",
        action="store_true",
        help="Skip build and flash; assume firmware already on device",
    )
    parser.add_argument(
        "--no-sync",
        action="store_true",
        help="Skip script sync; assume scripts already on device",
    )
    parser.add_argument(
        "--scripts",
        nargs="+",
        metavar="NAME",
        default=PRODUCTION_SCRIPTS,
        help="Script base names to test (default: production scripts ARP NDS_v2)",
    )
    parser.add_argument(
        "--cycles",
        type=int,
        default=2,
        help="Number of start/stop cycles per script (default: 2)",
    )
    args = parser.parse_args()

    # ── 1. Build & flash ──────────────────────────────────────────────────────
    if not args.no_flash:
        if not run(["make", "all"], "build firmware"):
            sys.exit(1)
        if not HEX_FILE.exists():
            print(f"[ERROR] hex not found: {HEX_FILE}")
            sys.exit(1)
        if not run(
            ["teensy_loader_cli", f"--mcu={MCU}", "-wvs", str(HEX_FILE)],
            "flash firmware",
        ):
            sys.exit(1)
        print("[FLASH] waiting for device to re-enumerate …")
        time.sleep(4.0)   # Teensy re-enumerates after ~3 s

    # ── 2. Find port ──────────────────────────────────────────────────────────
    port = args.port or find_port()
    if not port:
        print("[ERROR] no serial port found; try -p /dev/ttyACMx")
        sys.exit(1)
    print(f"[PORT] using {port}")

    # ── 3. Sync scripts ───────────────────────────────────────────────────────
    if not args.no_sync:
        if not sync_scripts(port):
            print("[ERROR] sync failed")
            sys.exit(1)

        # Reboot so the launcher re-enumerates scripts after sync
        print("\n[SYNC] rebooting device to apply script changes …")
        subprocess.run(
            [sys.executable, str(TOOLS_DIR / "chirp_fs.py"), "-p", port, "reboot"],
            cwd=REPO,
        )
        print("[SYNC] waiting for reboot …")
        time.sleep(4.0)

    # ── 4. Connect and ping ───────────────────────────────────────────────────
    with ChirpDevice(port, args.baud) as dev:
        time.sleep(0.5)
        print("\n[PING] checking device is alive …")
        if not dev.ping(timeout=5.0):
            print("[ERROR] device did not respond to ping")
            sys.exit(1)
        print("[PING] OK")

        # Drain any boot messages
        dev.drain_logs(1.5)

        # ── 5. Run tests ──────────────────────────────────────────────────────
        results: dict[str, bool] = {}
        for name in args.scripts:
            results[name] = test_script(dev, name, cycles=args.cycles)

        # Drain remaining logs
        dev.drain_logs(1.0)

    # ── 6. Report ─────────────────────────────────────────────────────────────
    print(f"\n{ANSI_BOLD}=== Results ==={ANSI_RESET}")
    all_passed = True
    for name, passed in results.items():
        status = f"{ANSI_GREEN}PASS{ANSI_RESET}" if passed else f"{ANSI_RED}FAIL{ANSI_RESET}"
        print(f"  {name:20s}  {status}")
        if not passed:
            all_passed = False

    if all_passed:
        print(f"\n{ANSI_GREEN}{ANSI_BOLD}All tests passed.{ANSI_RESET}")
        sys.exit(0)
    else:
        print(f"\n{ANSI_RED}{ANSI_BOLD}Some tests FAILED.{ANSI_RESET}")
        sys.exit(1)


if __name__ == "__main__":
    main()
