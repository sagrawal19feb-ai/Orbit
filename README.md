# 🛰️ Orbit — Gemini voice assistant firmware (ESP32-S3)

A totally new, clean-room voice-assistant firmware for the Orbit device:
say **"Orbit"** → Gemini Live conversation → music · alarms & timers · weather · news.

## 📦 THE DELIVERABLE

**`orbit_firmware.bin`** — right here, at the project root.
Complete single-file image (bootloader + partitions + app + wake-word model),
**7.73 MB**, trimmed for fast uploads. Flash at offset `0x0`:

```bash
esptool.py --chip esp32s3 -b 921600 erase_flash     # once, coming from the old firmware
esptool.py --chip esp32s3 -b 921600 write_flash 0x0 orbit_firmware.bin
idf.py -p /dev/ttyACM0 monitor                      # boot log, 115200 baud
```

First boot: join the open WiFi **`Orbit-Setup`** → http://192.168.4.1 →
enter WiFi + Gemini API key + Jamendo ID + city.

**Hardware self-test:** hold **BOOT** while powering on → speaker tones, OLED
patterns, mic loopback (details in `firmware/release/RELEASE.md`).

## 📁 Layout

| Path | What |
|---|---|
| `orbit_firmware.bin` | **the deliverable** (flash this) |
| `firmware/` | the ESP-IDF v5.5.4 project — `main/` + 16 `components/`, `partitions.csv`, `prebuilt/assets.bin` |
| `firmware/release/` | flash parts + `FLASH.md` + `RELEASE.md` (test checklist) |
| `tools/` | `build_release.sh` (one-command rebuild), `ensure_idf.sh` (self-healing env), `backup_sources.sh` / `restore_sources.sh` |
| `docs/` | `FINAL_PLAN.md` (capability spec) · `MCP_PLAN.md` · `DISPLAY_DESIGN.md` · `BUILD_ENVIRONMENT.md` |
| `research/` | reverse-engineering notes (pack formats, xiaozhi refs) |
| `assets/models-from-device/` | the pre-trained mn6_en "Orbit" wake-word model files |
| `tests/` | host-test fixtures (MCP) |
| `orbit-sources.tar.gz` | single-file backup of all sources (survives sandbox snapshots) |
| `ROADMAP.md` | the original project plan |

## 🔧 Rebuild

```bash
bash tools/build_release.sh     # self-heals env → builds → regenerates orbit_firmware.bin here
```
