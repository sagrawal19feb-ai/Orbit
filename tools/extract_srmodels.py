#!/usr/bin/env python3
"""
Parse a xiaozhi/esp-sr srmodels.bin (pack_model.py format) and optionally extract sub-files.

Format (from 78/xiaozhi-assets-generator web/src/utils/WakenetModelPacker.js,
which mimics esp-sr pack_model.py):
    model_num: u32
    per model (model_info_t):
        model_name: char[32]
        file_number: u32
        per file:
            file_name: char[32]
            file_start: u32   (absolute offset within srmodels.bin)
            file_len:   u32
    then all file data concatenated (first file starts right after the header)

Usage: extract_srmodels.py <srmodels.bin> [outdir]
"""
import struct
import sys
import os


def parse(path, outdir=None):
    data = open(path, 'rb').read()
    model_num, = struct.unpack('<I', data[:4])
    print(f'== {path} ==')
    print(f'  size={len(data)} bytes, model_num={model_num}')
    off = 4
    models = []
    for _ in range(model_num):
        name = data[off:off + 32].split(b'\x00')[0].decode('utf-8', 'replace')
        off += 32
        fnum, = struct.unpack('<I', data[off:off + 4])
        off += 4
        files = []
        for _ in range(fnum):
            fname = data[off:off + 32].split(b'\x00')[0].decode('utf-8', 'replace')
            start, length = struct.unpack('<II', data[off + 32:off + 40])
            off += 40
            files.append((fname, start, length))
        models.append((name, files))
        print(f'  model {name!r}: {fnum} file(s)')
        for fname, start, length in files:
            print(f'      {fname!r:18} start=0x{start:08x}  len={length:>8}')
    header_len = off
    first_start = models[0][1][0][1] if models and models[0][1] else None
    print(f'  header ends at 0x{header_len:x}; first file starts at 0x{first_start:x} (match: {header_len == first_start})')
    total = header_len + sum(l for _, files in models for _, _, l in files)
    print(f'  computed total={total} vs actual={len(data)} (match: {total == len(data)})')

    if outdir:
        os.makedirs(outdir, exist_ok=True)
        for name, files in models:
            mdir = os.path.join(outdir, name)
            os.makedirs(mdir, exist_ok=True)
            for fname, start, length in files:
                content = data[start:start + length]
                assert len(content) == length, f'{name}/{fname} truncated'
                with open(os.path.join(mdir, fname), 'wb') as f:
                    f.write(content)
        print(f'  extracted sub-files to {outdir}')
    return models


if __name__ == '__main__':
    src = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else None
    parse(src, outdir)
