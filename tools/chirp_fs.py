#!/usr/bin/env python3
"""
chirp_fs.py - Host-side script filesystem tool for the Chirp device.

Frame format (matches firmware usb_serial_protocol.h):
  [0xAA][0x55][TYPE:u8][SEQ:u8][LEN:u16-LE][PAYLOAD:LEN bytes][CRC16:u16-LE]
  CRC16-CCITT (poly=0x1021, init=0xFFFF) covers TYPE..end of PAYLOAD.

Commands
--------
    ping                    Test connection
    reboot                  Request device reboot/reset
    bootloader              Request device reboot into HalfKay bootloader
    list                    List all scripts as /scripts/*.wren with byte size + filesystem space
    read  <path>            Print script source to stdout
    write <path> [file]     Write a script from <file> (or stdin if omitted)
    delete <path>           Delete a script
    stat  <path>            Show script size in bytes
    run   <path>            Execute a script at runtime
    monitor                 Watch device debug/log messages in real-time

Options
-------
  -p / --port PORT        Serial port          (default: /dev/ttyACM0)
  -b / --baud BAUD        Baud rate            (default: 115200)
  -t / --timeout SECS     Per-request timeout  (default: 5)

monitor Options
---------------
  -d / --duration SECS    Monitor for N seconds (0 = indefinite, Ctrl-C to stop)

Examples
--------
    chirp_fs.py list
    chirp_fs.py reboot
    chirp_fs.py bootloader
    chirp_fs.py write /scripts/arp.wren arp.wren
    chirp_fs.py read /scripts/arp.wren
    chirp_fs.py run /scripts/arp.wren
    cat patch.wren | chirp_fs.py write /scripts/patch.wren
    chirp_fs.py delete /scripts/arp.wren
    chirp_fs.py stat /scripts/builtin/_runtime.wren
    chirp_fs.py monitor                 # Indefinite; Ctrl-C to stop
    chirp_fs.py monitor -d 10           # Monitor for 10 seconds
    chirp_fs.py sync                    # Upload scripts/ and midi_maps/ to device
    chirp_fs.py sync scripts/           # Upload only scripts/
    chirp_fs.py sync scripts/ midi_maps/ --delete  # Sync and remove orphaned device files
"""

import argparse
import json
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional, Tuple

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

# ── Protocol constants ────────────────────────────────────────────────────────

FRAME_SYNC0       = 0xAA
FRAME_SYNC1       = 0x55
FRAME_OVERHEAD    = 8
FRAME_MAX_PAYLOAD = 4096
WRITE_CHUNK_MARKER = 0xFF

MSG_LOG_TEXT         = 0x01
MSG_PING             = 0x7E
MSG_PONG             = 0x7F
MSG_REBOOT_REQ       = 0x70
MSG_REBOOT_RESP      = 0x71
REBOOT_TOKEN         = b"RBT!"
BOOTLOADER_TOKEN     = b"BTLD"
MSG_FS_LIST_REQ      = 0x20
MSG_FS_LIST_RESP     = 0x21
MSG_FS_READ_REQ      = 0x22
MSG_FS_READ_RESP     = 0x23
MSG_FS_WRITE_REQ     = 0x24
MSG_FS_WRITE_RESP    = 0x25
MSG_FS_DELETE_REQ    = 0x26
MSG_FS_DELETE_RESP   = 0x27
MSG_FS_STAT_REQ      = 0x28
MSG_FS_STAT_RESP     = 0x29
MSG_FS_RUN_REQ       = 0x2A
MSG_FS_RUN_RESP      = 0x2B
MSG_FS_SPACE_REQ     = 0x2C
MSG_FS_SPACE_RESP    = 0x2D

STATUS_OK        = 0x00
STATUS_ERROR     = 0x01
STATUS_NOT_FOUND = 0x02
STATUS_TOO_LARGE = 0x03
STATUS_INVALID   = 0x04

_STATUS_MESSAGES = {
    STATUS_OK:        None,
    STATUS_ERROR:     "Device error",
    STATUS_NOT_FOUND: "Script not found",
    STATUS_TOO_LARGE: "Script too large for transfer",
    STATUS_INVALID:   "Invalid request",
}

# ── CRC16-CCITT ───────────────────────────────────────────────────────────────

def _crc16(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc

# ── Frame encode / decode ─────────────────────────────────────────────────────

def _build_frame(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    """Encode one frame."""
    if len(payload) > FRAME_MAX_PAYLOAD:
        raise ValueError(f"Payload too large ({len(payload)} > {FRAME_MAX_PAYLOAD})")
    # header: sync0 sync1 type seq len_lo len_hi
    header = struct.pack("<BBBBH", FRAME_SYNC0, FRAME_SYNC1, msg_type, seq, len(payload))
    crc = _crc16(header[2:] + payload)          # CRC over type+seq+len+payload
    return header + payload + struct.pack("<H", crc)


def _check_status(data: bytes) -> None:
    """Raise RuntimeError if the first byte is not STATUS_OK."""
    if not data:
        raise RuntimeError("Empty response payload")
    status = data[0]
    msg = _STATUS_MESSAGES.get(status, f"Unknown status 0x{status:02X}")
    if msg:
        raise RuntimeError(msg)


def _name_payload(name: str) -> bytes:
    nb = name.encode("utf-8")
    if len(nb) > 255:
        raise ValueError("Path too long (max 255 bytes)")
    return bytes([len(nb)]) + nb


def _normalize_device_path(path: str) -> str:
    normalized = path.strip()
    if not normalized:
        raise ValueError("Path cannot be empty")

    if "\\" in normalized:
        normalized = normalized.replace("\\", "/")

    if not normalized.startswith("/"):
        normalized = "/" + normalized

    while "//" in normalized:
        normalized = normalized.replace("//", "/")

    if ".." in normalized:
        raise ValueError("Path cannot contain '..'")

    if normalized.endswith("/") and len(normalized) > 1:
        normalized = normalized[:-1]

    return normalized


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _target_base_for_dir(local_dir: Path) -> str:
    value = str(local_dir).replace("\\", "/")
    if value.endswith("scripts/builtin"):
        return "/scripts/builtin"
    if value.endswith("midi_maps"):
        return "/userdata"
    if value.endswith("scripts"):
        return "/scripts/user"
    return "/scripts/user"


_BUILTIN_RUNTIME_ORDER = [
    "clock.wren",
    "debug.wren",
    "file.wren",
    "config.wren",
    "script.wren",
    "display.wren",
    "midi.wren",
    "_runtime.wren",
]


def _strip_import_lines(text: str) -> str:
    lines = []
    for line in text.splitlines():
        if line.lstrip().startswith("import "):
            continue
        lines.append(line)
    return "\n".join(lines).strip()


def _minify_wren_text(text: str) -> str:
    out = []
    for raw in text.splitlines():
        line = raw.rstrip()
        stripped = line.lstrip()
        if not stripped:
            continue
        if stripped.startswith("//"):
            continue
        out.append(line)
    return "\n".join(out).strip()


def _build_builtin_runtime_bundle(local_dir: Path) -> str:
    parts = []

    for name in _BUILTIN_RUNTIME_ORDER:
        source_path = local_dir / name
        if not source_path.is_file():
            raise FileNotFoundError(f"Missing builtin runtime source: {source_path}")
        source_text = source_path.read_text(encoding="utf-8")
        parts.append(_strip_import_lines(source_text))

    combined = "\n".join(parts)
    return _minify_wren_text(combined) + "\n"


def _build_readme_text() -> str:
    root = _repo_root()
    revision = "unknown"
    branch = "unknown"
    dirty = "unknown"

    try:
        revision = subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        pass

    try:
        branch = subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "--abbrev-ref", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        pass

    try:
        status = subprocess.check_output(
            ["git", "-C", str(root), "status", "--porcelain"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        dirty = "yes" if status.strip() else "no"
    except Exception:
        pass

    build_date = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    return (
        "Chirp Device Flash Manifest\n"
        "===========================\n\n"
        f"Build date: {build_date}\n"
        f"Git revision: {revision}\n"
        f"Git branch: {branch}\n"
        f"Git dirty: {dirty}\n\n"
        "Flash layout:\n"
        "- /scripts/user      User-managed Wren scripts (*.wren)\n"
        "- /scripts/builtin   Built-in scripts (runtime/modules)\n"
        "- /userdata          User data files (maps/config/etc.)\n"
        "- /README.txt        Build metadata on flash root\n"
    )


def _build_nds_flat_map(source_path: Path) -> str:
    raw = json.loads(source_path.read_text(encoding="utf-8"))
    lines = [
        "# NDS flat map generated from NDS_midi_map.json",
        "# kit|instrument|full_name|parameter|cc",
    ]
    for kit, instruments in raw.items():
        for instrument, params in instruments.items():
            full_name = params.get("full_name", instrument)
            for parameter, cc in params.items():
                if parameter == "full_name":
                    continue
                lines.append(f"{kit}|{instrument}|{full_name}|{parameter}|{cc}")
    return "\n".join(lines) + "\n"

# ── Client ────────────────────────────────────────────────────────────────────

class ChirpClient:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 5.0):
        self._port    = port
        self._baud    = baud
        self._timeout = timeout
        self._ser: Optional[serial.Serial] = None
        self._seq = 1  # 1-255; 0 is reserved for device-initiated log messages

    def connect(self) -> None:
        self._ser = serial.Serial(self._port, self._baud, timeout=0.05)

    def close(self) -> None:
        if self._ser and self._ser.is_open:
            self._ser.close()

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.close()

    # ── Low-level frame I/O ───────────────────────────────────────────────────

    def _next_seq(self) -> int:
        seq = self._seq
        self._seq = (self._seq % 255) + 1   # wraps 1..255
        return seq

    def _read_frame(self, timeout: float) -> Optional[Tuple[int, int, bytes]]:
        """
        Block until a valid frame arrives or timeout expires.
        Returns (type, seq, payload) on success, None on timeout/error.
        Log frames (MSG_LOG_TEXT) are printed to stderr automatically.
        """
        deadline = time.monotonic() + timeout
        state = 0          # 0=SYNC0 1=SYNC1 2=HEADER 3=PAYLOAD 4=CRC
        hdr       = bytearray()
        payload   = bytearray()
        crc_bytes = bytearray()

        while time.monotonic() < deadline:
            remaining = max(0.001, deadline - time.monotonic())
            self._ser.timeout = min(remaining, 0.05)
            raw = self._ser.read(1)
            if not raw:
                continue
            b = raw[0]

            if state == 0:                          # SYNC0
                if b == FRAME_SYNC0:
                    state = 1

            elif state == 1:                        # SYNC1
                if b == FRAME_SYNC1:
                    hdr.clear(); state = 2
                else:
                    state = 0

            elif state == 2:                        # HEADER (4 bytes)
                hdr.append(b)
                if len(hdr) == 4:
                    plen = hdr[2] | (hdr[3] << 8)
                    if plen > FRAME_MAX_PAYLOAD:
                        state = 0; continue
                    payload.clear()
                    crc_bytes.clear()
                    state = 3 if plen > 0 else 4

            elif state == 3:                        # PAYLOAD
                payload.append(b)
                if len(payload) == (hdr[2] | (hdr[3] << 8)):
                    state = 4

            elif state == 4:                        # CRC (2 bytes)
                crc_bytes.append(b)
                if len(crc_bytes) == 2:
                    got = crc_bytes[0] | (crc_bytes[1] << 8)
                    exp = _crc16(bytes(hdr) + bytes(payload))
                    if got == exp:
                        msg_type = hdr[0]
                        seq      = hdr[1]
                        data     = bytes(payload)
                        if msg_type == MSG_LOG_TEXT:
                            print(f"[DEVICE] {data.decode('utf-8', errors='replace')}",
                                  file=sys.stderr)
                            # Keep reading; don't return log frames
                            hdr.clear(); payload.clear(); crc_bytes.clear()
                            state = 0
                            continue
                        return msg_type, seq, data
                    state = 0   # CRC mismatch

        return None

    def _request(self, msg_type: int, payload: bytes = b"",
                 timeout: Optional[float] = None) -> Tuple[int, bytes]:
        """
        Send a request and return (response_type, response_payload).
        Raises RuntimeError on timeout or unexpected failure.
        """
        if timeout is None:
            timeout = self._timeout
        seq = self._next_seq()
        self._ser.write(_build_frame(msg_type, seq, payload))

        deadline = time.monotonic() + timeout
        for _ in range(64):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            resp = self._read_frame(remaining)
            if resp is None:
                break
            rtype, rseq, rdata = resp
            if rseq == seq:
                return rtype, rdata
            # Non-matching seq (shouldn't happen normally) – keep looking

        raise RuntimeError("Timeout waiting for response")

    # ── Public API ───────────────────────────────────────────────────────────

    def ping(self) -> float:
        """Ping the device. Returns round-trip time in seconds."""
        t0 = time.monotonic()
        rtype, _ = self._request(MSG_PING)
        rtt = time.monotonic() - t0
        if rtype != MSG_PONG:
            raise RuntimeError(f"Unexpected ping response: 0x{rtype:02X}")
        return rtt

    def reboot(self, bootloader: bool = False) -> None:
        """Request a device reboot or bootloader entry. Ack may be missed if reset occurs quickly."""
        seq = self._next_seq()
        token = BOOTLOADER_TOKEN if bootloader else REBOOT_TOKEN
        self._ser.write(_build_frame(MSG_REBOOT_REQ, seq, token))

        deadline = time.monotonic() + self._timeout
        while time.monotonic() < deadline:
            remaining = max(0.001, deadline - time.monotonic())
            resp = self._read_frame(remaining)
            if resp is None:
                break

            rtype, rseq, rdata = resp
            if rseq != seq:
                continue

            if rtype != MSG_REBOOT_RESP:
                raise RuntimeError(f"Unexpected reboot response: 0x{rtype:02X}")

            _check_status(rdata)
            return

        # Device may reset before response can be read; treat as success.
        return

    def list_files(self) -> list:
        """Return a list of managed file paths on the device."""
        rtype, data = self._request(MSG_FS_LIST_REQ)
        if rtype != MSG_FS_LIST_RESP:
            raise RuntimeError(f"Unexpected response: 0x{rtype:02X}")
        if len(data) < 2:
            return []
        count = data[0] | (data[1] << 8)
        names = []
        idx = 2
        for _ in range(count):
            if idx >= len(data):
                break
            nl = data[idx]; idx += 1
            names.append(_normalize_device_path(data[idx:idx + nl].decode("utf-8", errors="replace")))
            idx += nl
        return names

    def list_file_stats(self) -> list:
        """Return a list of (path, size_bytes) tuples."""
        names = sorted(self.list_files())
        files = []
        for path in names:
            size = self.stat_file(path)
            files.append((path, size))
        return files

    def read_file(self, path: str) -> str:
        """Return a device file as string."""
        path = _normalize_device_path(path)
        rtype, data = self._request(MSG_FS_READ_REQ, _name_payload(path))
        if rtype != MSG_FS_READ_RESP:
            raise RuntimeError(f"Unexpected response: 0x{rtype:02X}")
        _check_status(data)
        return data[1:].decode("utf-8", errors="replace")

    def write_file(self, path: str, source: str) -> None:
        """Write (create or overwrite) a file."""
        path = _normalize_device_path(path)
        name_bytes = path.encode("utf-8")
        src_bytes  = source.encode("utf-8")
        if len(name_bytes) > 255:
            raise ValueError("Path too long")
        total = 1 + len(name_bytes) + len(src_bytes)
        if total <= FRAME_MAX_PAYLOAD:
            payload = bytes([len(name_bytes)]) + name_bytes + src_bytes
            rtype, data = self._request(MSG_FS_WRITE_REQ, payload)
            if rtype != MSG_FS_WRITE_RESP:
                raise RuntimeError(f"Unexpected response: 0x{rtype:02X}")
            _check_status(data)
            return

        max_chunk = FRAME_MAX_PAYLOAD - 1 - len(name_bytes) - 2
        if max_chunk <= 0:
            raise ValueError("Path too long for chunked transfer")

        offset = 0
        chunk_index = 0
        total_len = len(src_bytes)
        while offset < total_len:
            chunk = src_bytes[offset:offset + max_chunk]
            offset += len(chunk)

            flags = 0
            if chunk_index > 0:
                flags |= 0x01  # append
            if offset >= total_len:
                flags |= 0x02  # finalize

            payload = (
                bytes([len(name_bytes)])
                + name_bytes
                + bytes([WRITE_CHUNK_MARKER, flags])
                + chunk
            )
            rtype, data = self._request(MSG_FS_WRITE_REQ, payload)
            if rtype != MSG_FS_WRITE_RESP:
                raise RuntimeError(f"Unexpected response: 0x{rtype:02X}")
            _check_status(data)
            chunk_index += 1

    def delete_file(self, path: str) -> None:
        """Delete a file."""
        path = _normalize_device_path(path)
        rtype, data = self._request(MSG_FS_DELETE_REQ, _name_payload(path))
        if rtype != MSG_FS_DELETE_RESP:
            raise RuntimeError(f"Unexpected response: 0x{rtype:02X}")
        _check_status(data)

    def stat_file(self, path: str) -> int:
        """Return file size in bytes."""
        path = _normalize_device_path(path)
        rtype, data = self._request(MSG_FS_STAT_REQ, _name_payload(path))
        if rtype != MSG_FS_STAT_RESP:
            raise RuntimeError(f"Unexpected response: 0x{rtype:02X}")
        _check_status(data)
        if len(data) < 5:
            raise RuntimeError("Invalid stat response")
        return int.from_bytes(data[1:5], byteorder="little", signed=False)

    def get_space(self) -> dict:
        """Return filesystem info dict with keys: total, used, free, block_size, block_count, block_cycles, backend."""
        rtype, data = self._request(MSG_FS_SPACE_REQ)
        if rtype != MSG_FS_SPACE_RESP:
            raise RuntimeError(f"Unexpected response: 0x{rtype:02X}")
        _check_status(data)
        if len(data) < 9:
            raise RuntimeError("Invalid space response")
        total = int.from_bytes(data[1:5], byteorder="little", signed=False)
        used  = int.from_bytes(data[5:9], byteorder="little", signed=False)
        info  = {"total": total, "used": used, "free": total - used}
        if len(data) >= 21:
            info["block_size"]   = int.from_bytes(data[9:13],  byteorder="little", signed=False)
            info["block_count"]  = int.from_bytes(data[13:17], byteorder="little", signed=False)
            info["block_cycles"] = int.from_bytes(data[17:21], byteorder="little", signed=True)
        if len(data) >= 22:
            backend_code = data[21]
            backend_name = {
                0: "none",
                1: "sd",
                2: "littlefs",
            }.get(backend_code, f"unknown({backend_code})")
            info["backend"] = backend_name
        return info

    def run_script(self, path: str) -> str:
        """Execute a .wren script at runtime and return a success message."""
        path = _normalize_device_path(path)
        rtype, data = self._request(MSG_FS_RUN_REQ, _name_payload(path))
        if rtype != MSG_FS_RUN_RESP:
            raise RuntimeError(f"Unexpected response: 0x{rtype:02X}")
        if len(data) < 1:
            raise RuntimeError("Invalid run response")
        status = data[0]
        if status == STATUS_OK:
            return "Script executed successfully"
        if status == STATUS_ERROR:
            message = data[1:].decode("utf-8", errors="replace").strip()
            raise RuntimeError(message or "Script execution failed")
        message = _STATUS_MESSAGES.get(status, f"Unknown status 0x{status:02X}")
        raise RuntimeError(message)

    def _listen_for_logs(self, timeout: float) -> Optional[str]:
        """
        Block until a log frame arrives or timeout expires.
        Prints log to stderr and returns the log text.
        Returns None if timeout.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = max(0.001, deadline - time.monotonic())
            resp = self._read_frame(remaining)
            if resp is None:
                return None
            rtype, rseq, rdata = resp
            if rtype == MSG_LOG_TEXT:
                text = rdata.decode("utf-8", errors="replace")
                print(f"[DEVICE] {text}", file=sys.stderr)
                return text
        return None

    def sync_files(self, dirs: list, extensions: list = None,
                    delete: bool = False, verbose: bool = True):
        """
        Upload all matching files from local directories to the device.

                Layout mapping:
                    scripts/         -> /scripts/user/
                    scripts/builtin/ -> /scripts/builtin/
                    midi_maps/       -> /userdata/

                All filenames are uploaded with their full extensions unchanged.

        Returns (uploaded, deleted, errors) counts.
        """
        if extensions is None:
            extensions = [".wren", ".json"]

        local_files: dict = {}  # device_path -> Path
        generated_files: dict = {}  # device_path -> (label, source)
        for d in dirs:
            p = Path(d)
            if not p.is_dir():
                print(f"Warning: directory '{d}' not found, skipping", file=sys.stderr)
                continue
            target_base = _target_base_for_dir(p)

            if target_base == "/userdata" and p.name == "midi_maps":
                nds_json = p / "NDS_midi_map.json"
                if nds_json.is_file():
                    generated_files["/userdata/NDS_midi_map.txt"] = (
                        "<generated NDS flat map>",
                        _build_nds_flat_map(nds_json),
                    )

            if target_base == "/scripts/builtin" and p.name == "builtin":
                generated_files["/scripts/builtin/_runtime.wren"] = (
                    "<generated builtin runtime>",
                    _build_builtin_runtime_bundle(p),
                )
                continue

            for f in sorted(p.iterdir()):
                if not f.is_file() or f.suffix not in extensions:
                    continue
                device_path = _normalize_device_path(f"{target_base}/{f.name}")
                local_files[device_path] = f

        # Always publish a firmware metadata README to flash.
        generated_files["/README.txt"] = ("<generated README>", _build_readme_text())

        deleted = 0
        if delete:
            managed_prefixes = ("/scripts/user/", "/scripts/builtin/", "/userdata/")
            existing_paths = set(self.list_files())
            wanted_paths = set(local_files.keys()) | set(generated_files.keys())
            # Delete first so uploads have maximum free space available.
            for path in sorted(existing_paths - wanted_paths):
                if path == "/README.txt":
                    pass
                elif not path.startswith(managed_prefixes):
                    continue
                try:
                    self.delete_file(path)
                    if verbose:
                        print(f"  deleted {path}")
                    deleted += 1
                except Exception as e:
                    print(f"  ERROR deleting {path}: {e}", file=sys.stderr)

        uploaded = errors = 0
        all_paths = sorted(set(local_files.keys()) | set(generated_files.keys()))
        for device_path in all_paths:
            generated = generated_files.get(device_path)
            local_path = local_files.get(device_path)
            if generated is not None:
                src_name, source = generated
            else:
                src_name = str(local_path)
                source = local_path.read_text(encoding="utf-8")
            try:
                self.write_file(device_path, source)
                if verbose:
                    print(f"  {src_name}  ->  {device_path}"
                          f"  ({len(source.encode())} bytes)")
                uploaded += 1
            except Exception as e:
                label = src_name
                print(f"  ERROR uploading {label}: {e}", file=sys.stderr)
                errors += 1

        return uploaded, deleted, errors

    def monitor_logs(self, timeout: float = 0.0) -> None:
        """
        Monitor incoming log messages indefinitely (or until timeout > 0).
        Prints each log to stderr.
        """
        deadline = time.monotonic() + timeout if timeout > 0 else float("inf")
        while time.monotonic() < deadline:
            remaining = max(0.001, deadline - time.monotonic())
            resp = self._read_frame(remaining)
            if resp:
                rtype, rseq, rdata = resp
                # Non-log frames are just discarded during monitoring
                if rtype == MSG_LOG_TEXT:
                    text = rdata.decode("utf-8", errors="replace")
                    print(f"[DEVICE] {text}", file=sys.stderr)


# ── CLI ───────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        prog="chirp_fs",
        description="Host-side script filesystem tool for the Chirp device.",
    )
    parser.add_argument("-p", "--port",    default="/dev/ttyACM0")
    parser.add_argument("-b", "--baud",    type=int, default=115200)
    parser.add_argument("-t", "--timeout", type=float, default=5.0,
                        help="Per-request timeout in seconds")

    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ping",   help="Test connection")
    sub.add_parser("reboot", help="Request device reboot/reset")
    sub.add_parser("bootloader", help="Request device reboot into HalfKay bootloader")
    sub.add_parser("list",   help="List managed files on device")

    p_read = sub.add_parser("read", help="Print script file to stdout")
    p_read.add_argument("path")

    p_write = sub.add_parser("write", help="Write script file from local file or stdin")
    p_write.add_argument("path")
    p_write.add_argument("file", nargs="?", default=None,
                         help="Source file (omit to read from stdin)")

    p_del = sub.add_parser("delete", help="Delete a script file")
    p_del.add_argument("path")

    p_stat = sub.add_parser("stat", help="Show script file size")
    p_stat.add_argument("path")

    p_run = sub.add_parser("run", help="Execute a script at runtime")
    p_run.add_argument("path")

    p_monitor = sub.add_parser("monitor", help="Watch device debug/log messages")
    p_monitor.add_argument("-d", "--duration", type=float, default=0.0,
                           help="Monitor for N seconds (0 = indefinite, Ctrl-C to stop)")

    p_sync = sub.add_parser("sync",
                            help="Upload all local scripts and maps to device")
    p_sync.add_argument("dirs", nargs="*", default=["scripts/", "scripts/builtin/", "midi_maps/"],
                        help="Directories to sync (default: scripts/ scripts/builtin/ midi_maps/)")
    p_sync.add_argument("--ext", nargs="+", default=[".wren", ".json"],
                        metavar="EXT",
                        help="File extensions to include (default: .wren .json)")
    p_sync.add_argument("--delete", action="store_true",
                        help="Remove device files not present in local directories")
    p_sync.add_argument("--wait", type=float, default=0.0, metavar="SECS",
                        help="Seconds to wait before connecting (useful after a reboot)")

    args = parser.parse_args()

    try:
        if args.cmd == "monitor":
            print("Monitoring device logs... (Ctrl-C to stop)", file=sys.stderr)
            deadline = time.monotonic() + args.duration if args.duration > 0 else float("inf")

            while time.monotonic() < deadline:
                remaining = max(0.0, deadline - time.monotonic()) if args.duration > 0 else 0.0
                try:
                    with ChirpClient(args.port, args.baud, args.timeout) as client:
                        # Keep session bounded when duration is finite.
                        session_timeout = remaining if args.duration > 0 else 0.0
                        client.monitor_logs(timeout=session_timeout)
                        if args.duration > 0:
                            break
                except serial.SerialException as e:
                    print(f"Serial link dropped ({e}); reconnecting...", file=sys.stderr)
                    time.sleep(0.5)

            print("Monitor stopped.", file=sys.stderr)
            return

        with ChirpClient(args.port, args.baud, args.timeout) as client:

            if args.cmd == "ping":
                rtt = client.ping()
                print(f"PONG  ({rtt * 1000:.1f} ms)")

            elif args.cmd == "reboot":
                client.reboot()
                print("Reboot command sent")

            elif args.cmd == "bootloader":
                client.reboot(bootloader=True)
                print("Bootloader command sent")

            elif args.cmd == "list":
                files = client.list_file_stats()
                if not files:
                    print("(no managed files)")
                else:
                    for path, size in files:
                        print(f"{path}\t{size} bytes")
                try:
                    info = client.get_space()
                    total, used, free = info["total"], info["used"], info["free"]
                    pct = (used * 100 // total) if total > 0 else 0

                    def _fmt(n):
                        """Human-readable bytes: KB if >= 1024, else bytes."""
                        return f"{n // 1024} KB" if n >= 1024 else f"{n} B"

                    print()
                    print(f"Filesystem: {_fmt(used)} / {_fmt(total)} used ({pct}%),  {_fmt(free)} free")
                    if "backend" in info:
                        print(f"Backend: {info['backend']}")
                    if "block_size" in info:
                        bc = info["block_cycles"]
                        backend = info.get("backend", "")
                        if backend == "sd":
                            unit_label = "Clusters"
                            wear = "n/a"
                        else:
                            unit_label = "Blocks"
                            wear = f"{bc} erase cycles" if bc > 0 else "disabled"
                        used_blocks = used // info["block_size"] if info["block_size"] else 0
                        print(f"  {unit_label}: {info['block_size']:,} B × {info['block_count']}  |  "
                              f"Used: {used_blocks}/{info['block_count']}  |  "
                              f"Wear-leveling: {wear}")
                except Exception:
                    pass

            elif args.cmd == "read":
                source = client.read_file(args.path)
                sys.stdout.write(source)
                if source and not source.endswith("\n"):
                    sys.stdout.write("\n")

            elif args.cmd == "write":
                if args.file:
                    with open(args.file, "r", encoding="utf-8") as f:
                        source = f.read()
                else:
                    source = sys.stdin.read()
                target = _normalize_device_path(args.path)
                client.write_file(target, source)
                print(f"Written '{target}' ({len(source.encode())} bytes)")

            elif args.cmd == "delete":
                target = _normalize_device_path(args.path)
                client.delete_file(target)
                print(f"Deleted '{target}'")

            elif args.cmd == "stat":
                target = _normalize_device_path(args.path)
                size = client.stat_file(target)
                print(f"{target}: {size} bytes")

            elif args.cmd == "run":
                target = _normalize_device_path(args.path)
                result = client.run_script(target)
                print(f"Executed '{target}': {result}")

            elif args.cmd == "sync":
                if args.wait > 0:
                    print(f"Waiting {args.wait:.0f}s for device to come up...",
                          file=sys.stderr)
                    time.sleep(args.wait)
                uploaded, deleted, errors = client.sync_files(
                    args.dirs, args.ext, args.delete
                )
                print(f"Sync complete: {uploaded} uploaded, {deleted} deleted,"
                      f" {errors} error(s)")
                if errors:
                    sys.exit(1)

            elif args.cmd == "monitor":
                # Handled above to support automatic reconnect.
                pass

    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except serial.SerialException as e:
        print(f"Serial error: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
