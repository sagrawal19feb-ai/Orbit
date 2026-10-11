#!/usr/bin/env python3
"""
Pack files into a xiaozhi assets.bin (inverse of extract_assets.py).

Format (xiaozhi-assets-generator SpiffsGenerator.js):
    header (12 B): total_files u32 | checksum u32 | combined_data_length u32
    mmap_table per file (44 B): name char[32] | size u32 | offset u32 | width u16 | height u16
    file_data: per file: 0x5A 0x5A marker + raw bytes; `offset` is relative to the
               start of the data region and points at the marker
    combined_data_length = everything after the 12-byte header (table + data)
    checksum = 16-bit sum of all bytes after the 12-byte header

Usage: pack_assets.py <out.bin> <file1> [file2 ...]
"""
import struct
import sys
import os

MARKER = b'\x5a\x5a'
ENTRY = 44
HEADER = 12


def _pack_str(s, n):
    b = s.encode('utf-8')[:n]
    return b + b'\x00' * (n - len(b))


def pack(files):
    """files: [(name, bytes)] -> assets.bin bytes"""
    table_len = len(files) * ENTRY
    data_region = bytearray()
    entries = []
    for name, content in files:
        entries.append((name, len(content), len(data_region)))
        data_region += MARKER + content
    combined_len = table_len + len(data_region)
    blob = bytearray(struct.pack('<III', len(files), 0, combined_len))
    for name, size, off in entries:
        blob += _pack_str(name, 32)
        blob += struct.pack('<IIHH', size, off, 0, 0)
    blob += data_region
    checksum = sum(blob[HEADER:]) & 0xFFFF
    blob[4:8] = struct.pack('<I', checksum)
    return bytes(blob)


def main():
    out_path = sys.argv[1]
    files = []
    for p in sys.argv[2:]:
        with open(p, 'rb') as fh:
            files.append((os.path.basename(p), fh.read()))
    blob = pack(files)
    with open(out_path, 'wb') as fh:
        fh.write(blob)
    print(f'wrote {out_path} ({len(blob)} bytes, {len(files)} file(s))')


if __name__ == '__main__':
    main()
