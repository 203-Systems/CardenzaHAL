#!/usr/bin/env python3
"""Upload a Cardenza app through the 203 Launcher serial protocol.

See README.md beside this script for setup, metadata registration and recovery.
Derived from 203-Systems/203-launcher tools/serial_flasher.py (MIT).
"""

import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import time

# The device may emit a stray byte mid-UTF-8-sequence right when the serial
# port is opened; decode(errors="replace") turns that into U+FFFD, which some
# Windows consoles (cp1252) can't print. Make stdout tolerant of that instead
# of crashing the whole flash run over a cosmetic log line.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

try:
    import serial
except ImportError:
    serial = None


def require_serial():
    if serial is None:
        raise ValueError("Missing pyserial. Run: python -m pip install -r tools/requirements.txt")

READY_PREFIX = "READY"

# Launcher protocol is inherited from upstream; image validation here also
# checks the checksum and appended hash before opening a serial port.

ESP_IMAGE_HEADER_MAGIC = 0xE9
ESP_IMAGE_MAX_SEGMENTS = 16
PART_TABLE_OFFSET = 0x8000
PART_TABLE_SIZE = 0x1000
PART_ENTRY_SIZE = 0x20


def _align_up(value, alignment):
    return ((value + alignment - 1) // alignment) * alignment


def measure_esp_image_size(data, image_offset):
    """Walks an ESP app image's segment headers to find its true end offset
    (declared partition sizes are a ceiling; the real image is usually shorter).
    Returns 0 if there's no valid image header at image_offset."""
    if image_offset + 24 > len(data):
        return 0
    magic = data[image_offset]
    segment_count = data[image_offset + 1]
    hash_appended = data[image_offset + 23]
    if magic != ESP_IMAGE_HEADER_MAGIC or segment_count == 0 or segment_count > ESP_IMAGE_MAX_SEGMENTS:
        return 0
    cursor = image_offset + 24
    for _ in range(segment_count):
        if cursor + 8 > len(data):
            return 0
        segment_size = struct.unpack_from("<I", data, cursor + 4)[0]
        cursor += 8
        if segment_size > len(data) or cursor > len(data) - segment_size:
            return 0
        cursor += segment_size
    # ESP-IDF stores the XOR checksum at the LAST byte of a 16-byte block.
    end = _align_up(cursor + 1, 16)
    if hash_appended not in (0, 1) or end > len(data):
        return 0
    checksum = 0xEF
    pos = image_offset + 24
    for _ in range(segment_count):
        length = struct.unpack_from("<I", data, pos + 4)[0]
        pos += 8
        for byte in data[pos:pos + length]:
            checksum ^= byte
        pos += length
    if data[end - 1] != checksum:
        return 0
    if hash_appended:
        digest = hashlib.sha256(data[image_offset:end]).digest()
        if data[end:end + 32] != digest:
            return 0
        end += 32
    return end - image_offset


def detect_app_region(data):
    """Select one valid ESP32-S3 app, or reject malformed/ambiguous input.

    Full-flash images must start at flash address zero with a partition table
    at 0x8000. Only the selected app is sent; no bootloader, table or data.
    """
    merged = data[PART_TABLE_OFFSET:PART_TABLE_OFFSET + 2] == b"\xaa\x50"
    candidates = []
    if merged:
        for pos in range(PART_TABLE_OFFSET, PART_TABLE_OFFSET + PART_TABLE_SIZE, PART_ENTRY_SIZE):
            entry = data[pos:pos + PART_ENTRY_SIZE]
            if len(entry) != PART_ENTRY_SIZE:
                raise ValueError("Truncated partition table")
            if entry[:2] in (b"\xeb\xeb", b"\xff\xff"):
                break
            if entry[:2] != b"\xaa\x50":
                raise ValueError("Invalid partition table entry")
            ptype, subtype = entry[2:4]
            offset, capacity = struct.unpack_from("<II", entry, 4)
            if ptype != 0 or subtype not in (0, 0x20, *range(0x10, 0x20)):
                continue
            size = measure_esp_image_size(data, offset)
            if size and size <= capacity:
                candidates.append((offset, size))
        if len(candidates) != 1:
            raise ValueError("Merged BIN must contain exactly one valid app; use an app-only BIN")
        offset, size = candidates[0]
    else:
        offset, size = 0, measure_esp_image_size(data, 0)
    if not size:
        raise ValueError("Invalid or truncated ESP app image (checksum/SHA-256 failed)")
    if struct.unpack_from("<H", data, offset + 12)[0] != 9:
        raise ValueError("Expected an ESP32-S3 app built for Cardenza")
    return offset, size, merged


def reset_board(port):
    """Reset the board via esptool so it reboots into the Launcher's bootscreen."""
    print(f"[*] Resetting board on {port} via esptool...")
    cmd = [sys.executable, "-m", "esptool", "--port", port, "--after", "hard_reset", "run"]
    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
    except FileNotFoundError:
        sys.exit("esptool not found. Install it with: pip install esptool")
    except subprocess.CalledProcessError as exc:
        print(exc.stdout)
        print(exc.stderr)
        sys.exit(f"esptool failed to reset the board: {exc}")


class LineReader:
    """Buffers partial reads across calls so a line that arrives in the same
    chunk as (but after) the one a caller was waiting for is never dropped."""

    def __init__(self, ser):
        self.ser = ser
        self.buf = b""

    def wait_for_line(self, predicate, timeout, echo=True):
        deadline = time.monotonic() + timeout
        while True:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode(errors="replace").rstrip("\r")
                if echo and text:
                    print(f"    < {text}")
                if predicate(text):
                    return text
            if time.monotonic() >= deadline:
                return None
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                self.buf += chunk


def write_all(ser, data):
    """Handle short writes without changing the Launcher's 2048-byte ACK boundary."""
    view = memoryview(data)
    while view:
        written = ser.write(view)
        if written is None or written <= 0 or written > len(view):
            raise ValueError("Serial write made no progress")
        view = view[written:]


def positive_number(value):
    number = float(value)
    if not 0 < number < float("inf"):
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return number


def send_command(ser, command):
    print(f"[*] > {command}")
    write_all(ser, (command + "\n").encode())
    ser.flush()


def wait_for_launcher(ser, reader, timeout):
    # The one-shot boot banner may precede opening the port (--no-reset always
    # does). Query the existing version command instead of depending on it.
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        send_command(ser, "nav EscPress")
        send_command(ser, "version")
        if reader.wait_for_line(lambda line: line.startswith("Launcher "), min(1.0, max(0, deadline - time.monotonic()))):
            return True
    return False


def require_reply(reply, expected):
    if reply != expected:
        sys.exit(f"Unexpected device reply: {reply!r}; expected {expected!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-f", "--file", required=True, help="Path to the firmware .bin to flash")
    parser.add_argument("--check", action="store_true", help="Validate the BIN offline without connecting to a device")
    parser.add_argument("-p", "--port", help="Serial port, e.g. COM21 or /dev/ttyUSB0")
    parser.add_argument("--launcher", action="store_true", help="Update Software Launcher through a temporary free partition")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Serial baud rate (default 115200)")
    parser.add_argument("-n", "--name", default=None, help="Installation label hint (default: file name); does not register app identity")
    parser.add_argument(
        "--no-reset", action="store_true", help="Skip reset; connect to an already running Launcher"
    )
    parser.add_argument(
        "--boot-timeout", type=positive_number, default=15.0, help="Seconds to wait for the Launcher version response"
    )
    args = parser.parse_args()
    if args.baud <= 0:
        parser.error("--baud must be positive")

    if not os.path.isfile(args.file):
        sys.exit(f"File not found: {args.file}")
    name = args.name or re.sub(r"[^A-Za-z0-9._-]", "_", os.path.splitext(os.path.basename(args.file))[0])[:64]
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", name):
        sys.exit("App name must be 1-64 ASCII letters, digits, dots, underscores or hyphens")

    with open(args.file, "rb") as fh:
        file_data = fh.read()
    app_offset, size, is_merged = detect_app_region(file_data)
    if size == 0:
        sys.exit(f"Could not find a valid app image in {args.file}")
    if is_merged:
        print(
            f"[*] Detected merged image (bootloader+partitions+app); "
            f"flashing app portion only: offset=0x{app_offset:x} size={size} bytes"
        )
    else:
        print(f"[*] Detected plain app image: size={size} bytes")
    app_data = file_data[app_offset : app_offset + size]

    print(f"[*] App SHA-256: {hashlib.sha256(app_data).hexdigest()}")
    if args.check:
        print("[+] Image checks passed; board wiring compatibility still requires a Cardenza build.")
        return
    if not args.port:
        parser.error("--port is required unless --check is used")
    require_serial()
    if not args.no_reset:
        reset_board(args.port)

    print(f"[*] Opening {args.port} @ {args.baud}...")
    with serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=10) as ser:
        reader = LineReader(ser)
        print("[*] Checking that Launcher is ready...")
        if not wait_for_launcher(ser, reader, args.boot_timeout):
            sys.exit("Launcher did not respond. Return to its home screen and retry with --no-reset.")

        send_command(ser, "nav EscPress")
        time.sleep(0.3)

        print(f"[*] Flashing '{name}' ({size} bytes)...")
        role = "launcher" if args.launcher else "firmware"
        send_command(ser, f"flash {role} {name} {size}")

        reply = reader.wait_for_line(lambda line: line.startswith(READY_PREFIX) or line.startswith("ERR"), 10.0)
        if reply is None:
            sys.exit("Timed out waiting for READY from the device.")
        if reply.startswith("ERR"):
            sys.exit(f"Device rejected the flash request: {reply}")

        require_reply(reply, f"READY {size}")

        print("[*] Streaming firmware bytes...")
        data = app_data
        # Send one chunk, then wait for the device's ACK before sending the next.
        # A blind multi-second single write was observed to silently stall a few
        # percent short of completion on some Windows USB-serial drivers; keeping
        # only one chunk in flight at a time avoids that class of issue entirely.
        chunk_size = 2048
        sent = 0
        last_report = time.time()
        while sent < len(data):
            chunk = data[sent : sent + chunk_size]
            write_all(ser, chunk)
            ser.flush()
            sent += len(chunk)

            ack = reader.wait_for_line(
                lambda line: line.startswith("ACK") or line.startswith("ERR"), timeout=15.0, echo=False
            )
            if ack is None:
                sys.exit(f"Timed out waiting for ACK at {sent}/{len(data)} bytes.")
            if ack.startswith("ERR"):
                sys.exit(f"Device reported an error at {sent}/{len(data)} bytes: {ack}")

            require_reply(ack, f"ACK {sent}/{len(data)}")

            if time.time() - last_report > 1.0:
                print(f"    > {sent}/{len(data)} bytes")
                last_report = time.time()

        result = reader.wait_for_line(lambda line: line.startswith("OK") or line.startswith("ERR"), timeout=60.0)
        if result is None:
            sys.exit("Timed out waiting for the flash result.")
        if result.startswith("ERR"):
            sys.exit(f"Flash failed: {result}")

        require_reply(result, "OK flashed, rebooting")

        print(f"[+] Success: {result}")
        print("[*] Device is rebooting into the new firmware.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        sys.exit(f"Error: {exc}")
    except Exception as exc:
        if serial is not None and isinstance(exc, serial.SerialException):
            sys.exit(f"Serial error: {exc}")
        raise
    except KeyboardInterrupt:
        sys.exit("Cancelled; return to Launcher before retrying.")
