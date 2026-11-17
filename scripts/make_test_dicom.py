#!/usr/bin/env python3
"""生成用于回归测试的合成 DICOM（无第三方依赖，不含任何真实患者数据）。

生成内容：
  - 显式 VR 小端（1.2.840.10008.1.2.1）
  - 16 位 MONOCHROME2
  - Rescale Slope=1, Intercept=-1024, RescaleType=HU
  - WindowCenter=40, WindowWidth=400
  - 像素值 1000..1063 的水平渐变（HU = -24..39）

用法：
  python scripts/make_test_dicom.py out.dcm 20000 2000

注意：宽高会直接决定文件大小（16 位未压缩，w*h*2 字节）；20000x2000 约 80 MB。
"""

import struct
import sys


def elem(group: int, element: int, vr: bytes, value: bytes) -> bytes:
    tag = struct.pack("<HH", group, element)
    if vr in (b"OB", b"OW", b"OF", b"SQ", b"UT", b"UN"):
        return tag + vr + b"\x00\x00" + struct.pack("<I", len(value)) + value
    return tag + vr + struct.pack("<H", len(value)) + value


def ui(text: str) -> bytes:
    raw = text.encode().rstrip(b"\x00")
    return raw + (b"\x00" if len(raw) % 2 else b"")


def cs(text: str) -> bytes:
    raw = text.encode()
    return raw + (b" " if len(raw) % 2 else b"")


def ds(text: str) -> bytes:
    return cs(text)


def write_dicom(path: str, width: int, height: int) -> None:
    sop = "1.2.840.10008.5.1.4.1.1.7"  # Secondary Capture
    instance = "1.2.826.0.1.3680043.10.1000.1"
    implementation = "1.2.826.0.1.3680043.10.1000"
    transfer_syntax = "1.2.840.10008.1.2.1"  # Explicit VR Little Endian

    meta = b""
    meta += elem(0x0002, 0x0001, b"OB", b"\x00\x01")
    meta += elem(0x0002, 0x0002, b"UI", ui(sop))
    meta += elem(0x0002, 0x0003, b"UI", ui(instance))
    meta += elem(0x0002, 0x0010, b"UI", ui(transfer_syntax))
    meta += elem(0x0002, 0x0012, b"UI", ui(implementation))
    meta = elem(0x0002, 0x0000, b"UL", struct.pack("<I", len(meta))) + meta

    dataset = b""
    dataset += elem(0x0008, 0x0016, b"UI", ui(sop))
    dataset += elem(0x0008, 0x0018, b"UI", ui(instance))
    dataset += elem(0x0008, 0x0060, b"CS", cs("OT"))
    dataset += elem(0x0028, 0x0002, b"US", struct.pack("<H", 1))
    dataset += elem(0x0028, 0x0004, b"CS", cs("MONOCHROME2"))
    dataset += elem(0x0028, 0x0010, b"US", struct.pack("<H", height))
    dataset += elem(0x0028, 0x0011, b"US", struct.pack("<H", width))
    dataset += elem(0x0028, 0x0100, b"US", struct.pack("<H", 16))
    dataset += elem(0x0028, 0x0101, b"US", struct.pack("<H", 16))
    dataset += elem(0x0028, 0x0102, b"US", struct.pack("<H", 15))
    dataset += elem(0x0028, 0x0103, b"US", struct.pack("<H", 0))
    dataset += elem(0x0028, 0x1050, b"DS", ds("40"))
    dataset += elem(0x0028, 0x1051, b"DS", ds("400"))
    dataset += elem(0x0028, 0x1052, b"DS", ds("-1024"))
    dataset += elem(0x0028, 0x1053, b"DS", ds("1"))
    dataset += elem(0x0028, 0x1054, b"CS", cs("HU"))

    # 每行相同的水平渐变：存储值 1000..1063，对应 HU -24..39。
    row = struct.pack("<%dH" % width, *[1000 + (x * 64) // width for x in range(width)])
    pixels = bytearray(width * height * 2)
    for y in range(height):
        pixels[y * width * 2 : (y + 1) * width * 2] = row
    dataset += elem(0x7FE0, 0x0010, b"OW", bytes(pixels))

    with open(path, "wb") as stream:
        stream.write(b"\x00" * 128 + b"DICM" + meta + dataset)


def main() -> int:
    if len(sys.argv) != 4:
        print("用法: python scripts/make_test_dicom.py <输出.dcm> <宽> <高>", file=sys.stderr)
        return 2
    path = sys.argv[1]
    width = int(sys.argv[2])
    height = int(sys.argv[3])
    if width <= 0 or height <= 0:
        print("宽高必须为正数", file=sys.stderr)
        return 2
    write_dicom(path, width, height)
    print(f"{path}: {width}x{height} ({width * height * 2} 字节像素)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
