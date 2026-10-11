#!/usr/bin/env python3
"""
Build the Orbit assets image (firmware/prebuilt/assets.bin).

The `assets` partition holds the RAW esp-sr srmodels.bin (English MultiNet6
wake-word model "Orbit") — esp-sr's esp_srmodel_init("assets") mmaps the
partition and parses the srmodels.bin format directly. The old xiaozhi pack
format and the Chinese command lists (fst / commands_cn.txt) are dropped.

Output: firmware/prebuilt/assets.bin  ->  flash at offset 0x420000 (the `assets` partition)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack_srmodels

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MN6_EN_DIR = os.path.join(ROOT, 'assets', 'models-from-device', 'mn6_en')
OUT_DIR = os.path.join(ROOT, 'firmware', 'prebuilt')

MODEL_FILES = ['_MODEL_INFO_', 'mn6_data', 'mn6_index', 'vocab']


def main():
    models = {'mn6_en': {f: open(os.path.join(MN6_EN_DIR, f), 'rb').read() for f in MODEL_FILES}}
    srmodels = pack_srmodels.pack(models)
    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, 'assets.bin')
    with open(out, 'wb') as fh:
        fh.write(srmodels)
    print(f'==> {out} ({len(srmodels)} bytes)')
    print(f'    raw esp-sr srmodels.bin, model mn6_en only ("Orbit" wake word)')
    print(f'    flash with: esptool.py write_flash 0x420000 {out}')


if __name__ == '__main__':
    main()
