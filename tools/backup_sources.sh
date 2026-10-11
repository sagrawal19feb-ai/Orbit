#!/bin/bash
# 📦 Pack the authored sources into ONE small tarball (orbit-sources.tar.gz).
# A single ~4 MB file survives the workspace snapshot's size/file caps, so the
# project can always be restored (tools/restore_sources.sh) or rebuilt
# (tools/build_release.sh regenerates every binary, including orbit_firmware.bin).
set -e
cd "$(dirname "$0")/.."
OUT=orbit-sources.tar.gz
tar -czf "$OUT" \
    --exclude='firmware/build' \
    --exclude='firmware/managed_components' \
    --exclude='firmware/release' \
    --exclude='firmware/prebuilt' \
    --exclude='firmware/dependencies.lock' \
    --exclude='orbit-sources.tar.gz' \
    --exclude='orbit_firmware.bin' \
    firmware tools docs research tests assets ROADMAP.md README.md
echo "==> $OUT ($(du -h "$OUT" | cut -f1)) — restore with tools/restore_sources.sh"
