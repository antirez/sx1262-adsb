#!/usr/bin/env python3
"""Package a prebuilt ESP32-S3 receiver and the T-Echo image."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "firmware"


PROFILES = {
    "t3s3": {
        "build": "build",
        "installer": ROOT / "install/t3s3.py",
        "readme": ROOT / "install/T3-S3.txt",
        "archive": "t3s3-adsb.zip",
        "flash_settings": {"flash_mode": "dio", "flash_size": "4MB", "flash_freq": "80m"},
    },
    "tbeam-supreme": {
        "build": "build-tbeam-supreme",
        "installer": ROOT / "install/tbeam.py",
        "readme": ROOT / "install/T-Beam-Supreme.txt",
        "archive": "tbeam-supreme-adsb.zip",
        "flash_settings": {"flash_mode": "dio", "flash_size": "8MB", "flash_freq": "80m"},
    },
}


def package(board="t3s3"):
    profile = PROFILES[board]
    esp = ROOT / profile["build"]
    flasher_args = json.loads((esp / "flasher_args.json").read_text())
    if flasher_args["flash_settings"] != profile["flash_settings"]:
        raise ValueError("ESP-IDF flash settings changed; check the installer")
    layout = flasher_args["flash_files"]
    names = {0: "bootloader.bin", 0x8000: "partitions.bin", 0x10000: "receiver.bin"}
    if {int(k, 0) for k in layout} != set(names):
        raise ValueError("ESP-IDF flash layout changed; check the installer")
    files = {names[int(k, 0)]: esp / path for k, path in layout.items()}
    files["install.py"] = profile["installer"]
    files["installer.py"] = ROOT / "install/installer.py"
    files["README.txt"] = profile["readme"]
    files["NOTICES.txt"] = OUT / "NOTICES.txt"
    OUT.mkdir(exist_ok=True)
    bundle = OUT / profile["archive"]
    with ZipFile(bundle, "w") as zipfile:
        for name, path in sorted(files.items()):
            entry = ZipInfo(name)
            entry.compress_type = ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            zipfile.writestr(entry, path.read_bytes())
    if board != "t3s3":
        return bundle
    uf2 = OUT / "techo-adsb.uf2"
    shutil.copyfile(ROOT / "build-techo/techo-adsb.uf2", uf2)
    with (OUT / "SHA256SUMS").open("w") as checks:
        for path in (bundle, uf2):
            print(hashlib.sha256(path.read_bytes()).hexdigest(), " " + path.name, file=checks)
    return bundle

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", choices=PROFILES, default="t3s3")
    args = parser.parse_args()
    print(package(args.board))
