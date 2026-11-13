#!/usr/bin/env python3
"""Collect the two already-built board images into the user downloads."""
import hashlib
import json
from pathlib import Path
import shutil
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "firmware"

def package():
    esp = ROOT / "build"
    layout = json.loads((esp / "flasher_args.json").read_text())["flash_files"]
    names = {0: "bootloader.bin", 0x8000: "partitions.bin", 0x10000: "receiver.bin"}
    if {int(k, 0) for k in layout} != set(names):
        raise ValueError("ESP-IDF flash layout changed; check the installer")
    files = {names[int(k, 0)]: esp / path for k, path in layout.items()}
    files["install.py"] = ROOT / "install/t3s3.py"
    files["README.txt"] = ROOT / "install/T3-S3.txt"
    files["NOTICES.txt"] = OUT / "NOTICES.txt"
    OUT.mkdir(exist_ok=True)
    bundle = OUT / "t3s3-adsb.zip"
    with ZipFile(bundle, "w") as zipfile:
        for name, path in sorted(files.items()):
            entry = ZipInfo(name)
            entry.compress_type = ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            zipfile.writestr(entry, path.read_bytes())
    uf2 = OUT / "techo-adsb.uf2"
    shutil.copyfile(ROOT / "build-techo/techo-adsb.uf2", uf2)
    with (OUT / "SHA256SUMS").open("w") as checks:
        for path in (bundle, uf2):
            print(hashlib.sha256(path.read_bytes()).hexdigest(), " " + path.name, file=checks)

if __name__ == "__main__":
    package()
