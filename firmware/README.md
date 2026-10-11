# 🛰️ Orbit — Gemini Voice Assistant Firmware (ESP32-S3)

Clean-room firmware for the Orbit device (bread-compact-wifi pinout):
wake word **"Orbit"** (pre-trained MultiNet6 model) → **Gemini Live** conversation →
**music playback** (Jamendo full songs / Deezer previews / direct URLs) → **alarms & timers**.
No lamp (WS2812 on GPIO18 removed), no Chinese command assets.

## Layout

| Path | What |
|---|---|
| `main/` | app entry + state machine |
| `components/orbit_board` | pin config & hardware init (same pins as the original firmware) |
| `components/assets_reader` | reads the `assets` partition (xiaozhi pack format, zero-copy mmap) |
| `components/audio_pipeline` | I2S mic (16 kHz) + speaker (24 kHz) |
| `components/wake_word` | ESP-SR MultiNet6 "Orbit" detection |
| `components/gemini_client` | Gemini Live API (WebSocket, streaming audio) |
| `components/music_player` | search + HTTP stream + MP3 decode |
| `components/alarm_timer` | alarms & timers (NVS-persisted, RTC wake) |
| `components/orbit_display` | OLED status (SSD1306/SH1106 128×64) |
| `components/settings` | NVS settings (WiFi, Gemini key, Jamendo client ID, city) |
| `components/live_info` | live data: Open-Meteo weather · Google News RSS · SNTP time |
| `components/mcp_common` | MCP types + JSON-RPC 2.0 + SSE parser (pure C, host-tested) |
| `components/mcp_local` | device tools exposed as an in-process MCP server |
| `components/mcp_client` | remote MCP client (Streamable HTTP, schema cache, NVS config) |
| `components/mcp_router` | MCP↔Gemini bridge: allowlist, truncation, parallel calls |
| `prebuilt/assets.bin` | generated wake-word model image (flash at 0x800000) |

## Host tests (no hardware needed)

```bash
# from orbit-assistant/
gcc -I firmware/components/mcp_common/include -I tests/host \
    tests/host/test_mcp.c tests/host/cJSON.c \
    firmware/components/mcp_common/mcp_sse.c firmware/components/mcp_common/mcp_jsonrpc.c \
    firmware/components/mcp_common/mcp_common.c -o /tmp/test_mcp && /tmp/test_mcp
```

## Build

```bash
# 1. Install ESP-IDF v5.5.x for esp32s3 (once):
#    https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/
# 2. Export the environment:
. $HOME/esp-idf/export.sh
# 3. Build:
cd firmware
idf.py set-target esp32s3
idf.py build
```

## Flash

```bash
idf.py -p /dev/ttyACM0 flash                                        # app + partitions
esptool.py -p /dev/ttyACM0 write_flash 0x800000 prebuilt/assets.bin # wake-word model (once)
idf.py -p /dev/ttyACM0 monitor
```

## Regenerate the assets image (after changing the wake-word model)

```bash
python3 ../tools/build_assets.py
```
