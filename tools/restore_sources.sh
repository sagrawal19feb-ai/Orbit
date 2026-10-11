#!/bin/bash
# 📦 Restore authored sources from orbit-sources.tar.gz (self-heal after the
# workspace snapshot truncated them). Only extracts what is missing.
set -e
cd "$(dirname "$0")/.."
TB=orbit-sources.tar.gz
if [ ! -f "$TB" ]; then
    echo "no $TB — nothing to restore from" >&2
    exit 1
fi
# sanity: does the tarball contain the firmware sources?
if ! tar -tzf "$TB" | grep -q "firmware/main/app_main.c"; then
    echo "$TB does not contain the firmware sources — aborting" >&2
    exit 1
fi
echo "restoring sources from $TB …"
tar -xzf "$TB"
echo "sources restored"
