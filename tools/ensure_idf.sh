#!/bin/bash
# 🧰 Self-healing pre-build step — run before every build.
#
# The sandbox workspace snapshot caps at ~10k files. To stay under the cap the
# ESP-IDF tree lives in /home/user/.cache/esp-idf — `.cache` is excluded from
# workspace snapshots, so the 18k-file IDF tree never eats the file budget
# (which is what truncated the firmware sources in the first place). If the
# tree is missing or incomplete (e.g. wiped at a turn boundary), it is simply
# re-cloned — a shallow v5.5.4 clone takes ~10 s.
#
# This script ensures:
#   1. system packages (cmake, ninja, libusb)
#   2. ESP-IDF v5.5.4 tree at /home/user/.cache/esp-idf (fresh clone if needed)
#   3. component exclusions (submodule-heavy components we don't fetch)
#   4. submodules (re-clone at pinned SHAs if below 70% of expected files)
#   5. managed components (wipe if snapshot-truncated; idf.py re-downloads)
#   6. toolchain + IDF python venv (re-run install.sh if missing)
set -e
DST=/home/user/.cache/esp-idf
FW=/home/user/orbit-assistant/firmware

echo "=== ensure_idf: self-healing build environment ==="

# 0. authored sources (the snapshot cap can truncate them — restore from the
# single-file backup tarball orbit-sources.tar.gz, which survives any cap)
TB=/home/user/orbit-assistant/orbit-sources.tar.gz
SRC_MISSING=0
for f in firmware/main/app_main.c firmware/main/idf_component.yml firmware/main/CMakeLists.txt \
         firmware/CMakeLists.txt firmware/partitions.csv firmware/sdkconfig \
         firmware/components/wifi_config/wifi_config.c \
         firmware/components/gemini_client/gemini_client.c \
         firmware/components/music_player/music_player.c \
         firmware/components/alarm_timer/alarm_timer.c \
         firmware/components/minimp3/minimp3.h \
         firmware/components/wake_word/wake_word.c \
         tools/build_release.sh tools/ensure_idf.sh; do
    [ -f "/home/user/orbit-assistant/$f" ] || { SRC_MISSING=1; echo "missing source: $f"; }
done
if [ "$SRC_MISSING" = 1 ] && [ -f "$TB" ]; then
    echo "restoring sources from backup tarball"
    bash /home/user/orbit-assistant/tools/restore_sources.sh
fi

# 1. system packages
if ! which cmake > /dev/null 2>&1 || ! which ninja > /dev/null 2>&1; then
    echo "installing cmake/ninja/libusb"
    sudo apt-get install -y -qq cmake ninja-build libusb-1.0-0 libudev1 > /dev/null 2>&1
fi

# 2. ESP-IDF tree (fresh shallow clone if missing/incomplete)
if [ ! -f "$DST/components/esp_common/include/esp_idf_version.h" ] \
   || [ "$(find "$DST" -type f -not -path '*/.git/*' 2>/dev/null | wc -l)" -lt 15000 ]; then
    echo "cloning ESP-IDF v5.5.4 into $DST (shallow)"
    rm -rf "$DST"
    git clone --depth 1 --branch v5.5.4 https://github.com/espressif/esp-idf.git "$DST" > /dev/null 2>&1
fi
chmod +x "$DST/install.sh" "$DST/export.sh" "$DST/tools/idf.py" 2>/dev/null

# 3. remove components whose submodules we deliberately don't fetch
mkdir -p "$DST/components-disabled"
for c in mqtt bt openthread protobuf-c protocomm esp_local_ctrl wifi_provisioning cmock idf_test \
         esp_hid ieee802154; do
    if [ -d "$DST/components/$c" ]; then
        mv "$DST/components/$c" "$DST/components-disabled/$c" 2>/dev/null || rm -rf "$DST/components/$c"
    fi
done
echo "component exclusions applied"

# 4. managed components (snapshot cap can truncate them; the lock file then
# makes the component manager no-op — wipe and let idf.py re-download)
MC="$FW/managed_components"
if [ -d "$MC" ]; then
    MISSING=0
    for c in espressif__esp-sr espressif__esp-dsp espressif__dl_fft espressif__cjson espressif__esp_websocket_client; do
        [ -f "$MC/$c/CMakeLists.txt" ] || { MISSING=1; echo "managed component truncated: $c"; }
    done
    if [ "$MISSING" = 1 ]; then
        echo "wiping truncated managed_components + dependencies.lock (+ build cache)"
        rm -rf "$MC" "$FW/dependencies.lock" "$FW/build"
    fi
fi

# 5. submodules: re-clone at pinned SHA if below 70% of the expected file count
ensure_submodule() {
    local path="$1" url="$2" sha="$3" expected="$4"
    local n threshold
    n=$(find "$DST/$path" -type f -not -path '*/.git/*' 2>/dev/null | wc -l)
    threshold=$(( expected * 7 / 10 ))
    if [ "$n" -lt "$threshold" ]; then
        echo "re-cloning submodule $path ($n/$expected files)"
        rm -rf /tmp/submod_clone
        git init -q /tmp/submod_clone
        (cd /tmp/submod_clone && git remote add origin "$url" && git fetch -q --depth 1 origin "$sha" && git checkout -q FETCH_HEAD)
        rm -rf "$DST/$path"
        mkdir -p "$(dirname "$DST/$path")"
        mv /tmp/submod_clone "$DST/$path"
    fi
}
ensure_submodule components/lwip/lwip \
    https://github.com/espressif/esp-lwip.git fd432e4ee2cfb7f7f1c7eb7227e0173412e7b84e 756
ensure_submodule components/mbedtls/mbedtls \
    https://github.com/espressif/mbedtls.git ffb280bb63c78bfec1e1ab55040671768c85c923 1883
ensure_submodule components/json/cJSON \
    https://github.com/DaveGamble/cJSON.git c859b25da02955fef659d658b8f324b5cde87be3 229
ensure_submodule components/unity/unity \
    https://github.com/ThrowTheSwitch/Unity.git bf560290f6020737eafaa8b5cbd2177c3956c03f 176
ensure_submodule components/heap/tlsf \
    https://github.com/espressif/tlsf.git 2867f6883a12920b1969ff9624c0ab0e4185c2ce 8
ensure_submodule components/spiffs/spiffs \
    https://github.com/pellepl/spiffs.git 0dbb3f71c5f6fae3747a9d935372773762baf852 300
ensure_submodule components/esp_wifi/lib \
    https://github.com/espressif/esp32-wifi-lib.git b87cce81803b85c3639fb14bac6c24cff6d0fbad 70
ensure_submodule components/esp_phy/lib \
    https://github.com/espressif/esp-phy-lib.git 3d57415af6e4c92eff2c4c3463e20a51d7340aba 51
ensure_submodule components/esp_coex/lib \
    https://github.com/espressif/esp-coex-lib.git ee5cd79583c02f23e43e62931ffb55f5a4992d0f 16
ensure_submodule components/bootloader/subproject/components/micro-ecc/micro-ecc \
    https://github.com/kmackay/micro-ecc.git 24c60e243580c7868f4334a1ba3123481fe1aa48 7
echo "submodules OK"

# 6. toolchain + python venv (install.sh installs both into ~/.espressif)
VENV_PY=/home/user/.espressif/python_env/idf5.5_py3.13_env/bin/python
if [ ! -x /home/user/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc ] \
   || [ ! -x "$VENV_PY" ]; then
    echo "recreating IDF toolchain/venv (install.sh)"
    (cd "$DST" && ./install.sh esp32s3 > /tmp/idf_install.log 2>&1)
    echo "toolchain/venv ready"
fi
# export.sh audits that every tool in tools.json is installed — the snapshot cap
# can truncate ~/.espressif, so (re)install any missing tool by name.
if [ -x "$VENV_PY" ]; then
    for t in riscv32-esp-elf-gdb xtensa-esp-elf-gdb openocd-esp32 esp-rom-elfs esp32ulp-elf riscv32-esp-elf xtensa-esp-elf; do
        if ! ls /home/user/.espressif/tools/$t > /dev/null 2>&1; then
            echo "installing missing IDF tool: $t"
            IDF_PATH="$DST" "$VENV_PY" "$DST/tools/idf_tools.py" install "$t" > /dev/null 2>&1
        fi
    done
fi

echo "=== build environment OK ==="
