#!/usr/bin/env python3
"""
Extract files from a xiaozhi assets.bin (custom pack format).

Format (from 78/xiaozhi-assets-generator web/src/utils/SpiffsGenerator.js):
    header (12 bytes): total_files(u32) | checksum(u32) | combined_data_length(u32)
    mmap_table: total_files x { name[32] | size(u32) | offset(u32) | width(u16) | height(u16) }  (44 B each)
    file_data:  per file: 0x5A 0x5A marker + raw file bytes; `offset` is relative
                to the start of the data region (points at the 0x5A5A marker).

Usage: extract_assets.py <assets.bin> <outdir>
"""
import struct
import sys
import os

ENTRY_SIZE = 44
MARKER = b'\x5a\x5a'


def extract(path, outdir, blob=None):
    data = blob if blob is not None else open(path, 'rb').read()
    total_files, checksum, data_len = struct.unpack('<III', data[:12])
    print(f'== {path} ==')
    print(f'  total_files={total_files}  checksum=0x{checksum:04x}  data_len={data_len}  blob_size={len(data)}')
    table_off = 12
    data_start = table_off + total_files * ENTRY_SIZE
    print(f'  mmap_table @0x{table_off:x} ({total_files} entries), data region @0x{data_start:x}')

    # checksum is a 16-bit sum of bytes; try over the data region and over everything after the header
    csum_region = sum(data[data_start:]) & 0xFFFF
    csum_after_header = sum(data[12:]) & 0xFFFF
    print(f'  checksum verify: header=0x{checksum:04x}  sum(data_region)=0x{csum_region:04x}  sum(after_header)=0x{csum_after_header:04x}')

    os.makedirs(outdir, exist_ok=True)
    extracted = []
    for i in range(total_files):
        off = table_off + i * ENTRY_SIZE
        raw_name = data[off:off + 32]
        name = raw_name.split(b'\x00')[0].decode('utf-8', 'replace')
        size, foff, w, h = struct.unpack('<IIHH', data[off + 32:off + 44])
        marker = data[data_start + foff:data_start + foff + 2]
        status = 'OK' if marker == MARKER else f'BAD MARKER {marker.hex()}'
        print(f'  [{i}] {name!r:20} size={size:>9}  offset=0x{foff:08x}  w={w} h={h}  marker:{status}')
        if marker != MARKER:
            print(f'      !! skipping {name} (marker mismatch)')
            continue
        content = data[data_start + foff + 2:data_start + foff + 2 + size]
        assert len(content) == size, f'{name}: truncated ({len(content)} != {size})'
        out = os.path.join(outdir, name)
        with open(out, 'wb') as f:
            f.write(content)
        print(f'      -> {out} ({size} bytes)')
        extracted.append((name, size, out))
    return extracted


if __name__ == '__main__':
    src, outdir = sys.argv[1], sys.argv[2]
    if src.endswith('.bin') and os.path.getsize(src) > 0x900000:
        # full flash dump: slice out the assets partition (offset 0x800000, size 0x800000)
        fw = open(src, 'rb').read()
        blob = fw[0x800000:0x800000 + 0x800000]
        # trim trailing 0xFF padding
        trimmed = blob.rstrip(b'\xff')
        extract(src + ' [assets partition]', outdir, blob=trimmed)
    else:
        extract(src, outdir)
