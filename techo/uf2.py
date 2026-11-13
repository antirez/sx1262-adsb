"""Package only application flash. Reject a HEX that addresses the SoftDevice or bootloader."""
import struct
import sys
from pathlib import Path

def convert(source, dest):
    pages = {}
    base = 0
    end = False
    for line in source.read_text().splitlines():
        if not line.startswith(":") or end:
            raise ValueError("Unexpected Intel HEX record")
        data = bytes.fromhex(line[1:])
        size, address, kind = struct.unpack(">BHB", data[:4])
        if len(data) != size + 5 or sum(data) & 255:
            raise ValueError("Invalid Intel HEX checksum or length")
        body = data[4:-1]
        if kind in (2, 4):
            if size != 2 or address:
                raise ValueError("Invalid extended HEX address")
            base = int.from_bytes(body, "big") << (4 if kind == 2 else 16)
        elif kind == 0:
            for offset, value in enumerate(body, base + address):
                if not 0x26000 <= offset < 0xED000:
                    raise ValueError(f"Outside T-Echo application flash: {offset:#x}")
                pages.setdefault(offset & ~255, bytearray(b"\xff"*256))[offset & 255] = value
        elif kind == 1:
            end = True
        elif kind not in (3, 5):
            raise ValueError(f"Unsupported Intel HEX record {kind}")
    if not end or 0x26000 not in pages:
        raise ValueError("Missing application vectors or end record")
    with dest.open("wb") as f:
        for i, addr in enumerate(sorted(pages)):
            header = struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x2000, addr,
                                 256, i, len(pages), 0xADA52840)
            f.write(header + pages[addr] + bytes(220) + struct.pack("<I", 0x0AB16F30))

if __name__ == "__main__":
    convert(Path(sys.argv[1]), Path(sys.argv[2]))
