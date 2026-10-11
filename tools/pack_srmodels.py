#!/usr/bin/env python3
"""
Pack model files into an esp-sr srmodels.bin (inverse of extract_srmodels.py).

Format (esp-sr pack_model.py / xiaozhi WakenetModelPacker):
    model_num: u32
    per model (sorted by name):
        model_name: char[32]
        file_count: u32
        per file (sorted by name): file_name: char[32] | file_start: u32 | file_len: u32
    then all file data concatenated

Usage:
    pack_srmodels.py <out.bin> [--files-root DIR] <model_name>=<file1>,<file2>,...
Example:
    pack_srmodels.py srmodels.bin --files-root assets/models-from-device/mn6_en \
        mn6_en=_MODEL_INFO_,mn6_data,mn6_index,vocab
"""
import struct
import sys
import os


def _pack_str(s, n):
    b = s.encode('ascii')[:n]
    return b + b'\x00' * (n - len(b))


def pack(models):
    """models: {model_name: {file_name: bytes}} -> srmodels.bin bytes"""
    sorted_models = sorted(models.items())
    header_len = 4
    for _, files in sorted_models:
        header_len += 32 + 4 + len(files) * 40
    out = bytearray()
    out += struct.pack('<I', len(sorted_models))
    data_offset = header_len
    for name, files in sorted_models:
        out += _pack_str(name, 32)
        out += struct.pack('<I', len(files))
        for fname, fdata in sorted(files.items()):
            out += _pack_str(fname, 32)
            out += struct.pack('<II', data_offset, len(fdata))
            data_offset += len(fdata)
    for _, files in sorted_models:
        for _, fdata in sorted(files.items()):
            out += fdata
    return bytes(out)


def main():
    out_path = sys.argv[1]
    args = sys.argv[2:]
    root = os.getcwd()
    if '--files-root' in args:
        i = args.index('--files-root')
        root = args[i + 1]
        del args[i:i + 2]
    models = {}
    for spec in args:
        name, files = spec.split('=', 1)
        models[name] = {}
        for f in files.split(','):
            with open(os.path.join(root, f), 'rb') as fh:
                models[name][os.path.basename(f)] = fh.read()
    blob = pack(models)
    with open(out_path, 'wb') as fh:
        fh.write(blob)
    print(f'wrote {out_path} ({len(blob)} bytes, {len(models)} model(s))')


if __name__ == '__main__':
    main()
