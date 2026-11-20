#!/usr/bin/env python3
"""Install a prebuilt ADS-B receiver on a supported LilyGO ESP32-S3 board."""
import argparse
import datetime
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
IMAGES = [(0, "bootloader.bin"), (0x8000, "partitions.bin"), (0x10000, "receiver.bin")]
BOARDS = {
    "t3s3": {
        "name": "T3-S3",
        "flash_size": "4MB",
        "flash_bytes": 0x400000,
        "backup_prefix": "T3-S3-before-ADSB-",
        "check_flash_size": False,
    },
    "tbeam-supreme": {
        "name": "T-Beam Supreme SX1262",
        "flash_size": "8MB",
        "flash_bytes": 0x800000,
        "backup_prefix": "T-Beam-Supreme-before-ADSB-",
        "check_flash_size": True,
    },
}
CAPTURES_OFFSET = 0x100000
CAPTURES_SIZE = 0xF0000


def receiver_application(header):
    # The read starts at the partition table. The ESP-IDF app descriptor
    # follows the first image segment header, 32 bytes into the factory app.
    app = header[0x8000:]
    return (app[:1] == b"\xe9" and
            app[32:36] == bytes.fromhex("3254cdab") and
            app[80:112].split(b"\0", 1)[0] == b"lilygo_adsb")


def run(args, board):
    from serial.tools import list_ports

    profile = BOARDS[board]
    port = args.port
    if not port:
        ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x303a, 0x1001)]
        if len(ports) != 1:
            raise SystemExit("Hold BOOT, tap RESET, then release BOOT on the " + profile["name"] +
                             ". Connect one board or choose its port with --port PORT.")
        port = ports[0]
    for _, name in IMAGES:
        if not (HERE / name).is_file():
            raise SystemExit("Extract the whole ZIP first. Missing " + name)

    def flash(*words, reset=False, capture=False):
        result = subprocess.run(
            [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
             "--before", "no_reset", "--after", "hard_reset" if reset else "no_reset",
             *map(str, words)],
            check=True, text=True, capture_output=capture)
        if capture:
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            return result.stdout + result.stderr
        return ""

    if profile["check_flash_size"]:
        info = flash("flash_id", capture=True)
        found = re.search(r"Detected flash size:\s*(\S+)", info)
        if not found or found.group(1).upper() != profile["flash_size"]:
            actual = found.group(1) if found else "unknown"
            raise SystemExit("This package requires " + profile["flash_size"] +
                             " flash; detected " + actual + ". No flash was written.")

    with tempfile.TemporaryDirectory() as tmp:
        header = Path(tmp) / "header.bin"
        flash("read_flash", "0x8000", "0x8100", header)
        installed = header.read_bytes()
        existing = receiver_application(installed)
        if existing and not installed.startswith((HERE / "partitions.bin").read_bytes()):
            raise SystemExit("The receiver on this board uses a different partition layout. "
                             "Back it up before migrating its journal.")
    if not existing:
        timestamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        backup = Path.cwd() / (profile["backup_prefix"] + timestamp + ".bin")
        print("Saving the original firmware to", backup, flush=True)
        partial = backup.with_suffix(".partial")
        partial.touch(mode=0o600, exist_ok=False)
        flash("read_flash", 0, hex(profile["flash_bytes"]), partial)
        flash("verify_flash", 0, partial)
        partial.rename(backup)
        flash("erase_region", hex(CAPTURES_OFFSET), hex(CAPTURES_SIZE))
    args = [s for offset, name in IMAGES for s in (hex(offset), HERE / name)]
    flash("write_flash", "--flash_mode", "dio", "--flash_freq", "80m",
          "--flash_size", profile["flash_size"], *args, reset=True)
    print("Installed. If the OLED has not started, tap RESET once with BOOT released.")


def main(board):
    parser = argparse.ArgumentParser(description="Install the receiver on the " + BOARDS[board]["name"] + ".")
    parser.add_argument("--port", help="USB serial port, e.g. COM5 or /dev/ttyACM0")
    args = parser.parse_args()
    if not importlib.util.find_spec("esptool"):
        sys.exit("Install the USB flasher first: python -m pip install \"esptool>=4.7,<6\"")
    try:
        run(args, board)
    except subprocess.CalledProcessError as error:
        if error.stdout:
            sys.stdout.write(error.stdout)
        if error.stderr:
            sys.stderr.write(error.stderr)
        sys.exit(error.returncode)
