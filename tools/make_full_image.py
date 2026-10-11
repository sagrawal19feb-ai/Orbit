#!/usr/bin/env python3
"""
Assemble the standalone Orbit flash image (flash at offset 0x0).

Layout (see firmware/partitions.csv):
    0x0       bootloader.bin
    0x8000    partition-table.bin
    0xd000    ota_data_initial.bin
    0x20000   orbit.bin            (ota_0 app)
    0x420000  assets.bin           (wake-word model image)

Default output is TRIMMED to the last content byte (fast uploads).
With --full, writes the complete 16 MB image (erases everything).

Usage: make_full_image.py [output.bin] [--full]
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, 'firmware')
IMG_SIZE = 0x1000000  # 16 MB

PARTS = [
    (0x0,      'build/bootloader/bootloader.bin'),
    (0x8000,   'build/partition_table/partition-table.bin'),
    (0xd000,   'build/ota_data_initial.bin'),
    (0x20000,  'build/orbit.bin'),
    (0x420000, 'prebuilt/assets.bin'),
]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    full = '--full' in sys.argv
    out = args[0] if args else os.path.join(FW, 'release', 'orbit_firmware.bin')

    blobs = []
    end = 0
    for off, rel in PARTS:
        path = os.path.join(FW, rel)
        data = open(path, 'rb').read()
        blobs.append((off, data))
        end = max(end, off + len(data))
        print(f'  @0x{off:06x}  {os.path.basename(rel):22} {len(data):>8} bytes')

    if full:
        end = IMG_SIZE
    else:
        end = (end + 0xFFF) & ~0xFFF  # round up to a 4 KB flash sector

    img = bytearray(b'\xff' * end)
    for off, data in blobs:
        img[off:off + len(data)] = data

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(img)
    size_mb = len(img) / 1024 / 1024
    print(f'==> {out} ({len(img)} bytes = {size_mb:.2f} MB, flash at 0x0)'
          + (' [full 16 MB image]' if full else ' [trimmed to content]'))


if __name__ == '__main__':
    main()
