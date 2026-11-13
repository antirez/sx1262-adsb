#!/usr/bin/env python3
"""Install the prebuilt receiver on a 4 MB LilyGO T3-S3 SX1262."""
import argparse
import datetime
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
IMAGES = [(0, "bootloader.bin"), (0x8000, "partitions.bin"), (0x10000, "receiver.bin")]

def receiver_application(header):
    # The read starts at the partition table. The ESP-IDF app descriptor
    # follows the first image segment header, 32 bytes into the factory app.
    app = header[0x8000:]
    return (app[:1] == b"\xe9" and
            app[32:36] == bytes.fromhex("3254cdab") and
            app[80:112].split(b"\0", 1)[0] == b"lilygo_adsb")

def run(args):
    from serial.tools import list_ports
    port = args.port
    if not port:
        ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x303a, 0x1001)]
        if len(ports) != 1:
            raise SystemExit("Hold BOOT, tap RESET, then release BOOT on the T3-S3. "
                             "Connect one board or choose its port with --port PORT.")
        port = ports[0]
    for _, name in IMAGES:
        if not (HERE / name).is_file():
            raise SystemExit("Extract the whole ZIP first. Missing " + name)
    def flash(*words, reset=False):
        subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
                        "--before", "no_reset", "--after", "hard_reset" if reset else "no_reset",
                        *map(str, words)], check=True)
    with tempfile.TemporaryDirectory() as tmp:
        header = Path(tmp) / "header.bin"
        flash("read_flash", "0x8000", "0x8100", header)
        installed = header.read_bytes()
        existing = receiver_application(installed)
        if existing and not installed.startswith((HERE / "partitions.bin").read_bytes()):
            raise SystemExit("The receiver on this board uses a different partition layout. "
                             "Back it up before migrating its journal.")
    if not existing:
        # First install replaces another program and its journal area. Keep
        # the original 4 MB image and check it against flash before erasing.
        timestamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        backup = Path.cwd() / ("T3-S3-before-ADSB-" + timestamp + ".bin")
        print("Saving the original firmware to", backup, flush=True)
        partial = backup.with_suffix(".partial")
        partial.touch(mode=0o600, exist_ok=False)
        flash("read_flash", 0, "0x400000", partial)
        flash("verify_flash", 0, partial)
        partial.rename(backup)
        flash("erase_region", "0x100000", "0xf0000")
    args = [s for offset, name in IMAGES for s in (hex(offset), HERE / name)]
    flash("write_flash", "--flash_mode", "dio", "--flash_freq", "80m",
          "--flash_size", "4MB", *args, reset=True)
    print("Installed. If the OLED has not started, tap RESET once with BOOT released.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB serial port, e.g. COM5 or /dev/ttyACM0")
    args = parser.parse_args()
    if not importlib.util.find_spec("esptool"):
        sys.exit("Install the USB flasher first: python -m pip install \"esptool>=4.7,<6\"")
    try:
        run(args)
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
