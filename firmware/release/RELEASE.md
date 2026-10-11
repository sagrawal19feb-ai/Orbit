# 🛰️ Orbit firmware — release notes & hardware test checklist

**Deliverable:** `orbit_firmware.bin` — at the **project root** (`orbit-assistant/orbit_firmware.bin`)
**Size:** 8,105,984 B = **7.73 MB** (trimmed to the last content byte — flash at `0x0`)
**SHA-256:** `32ac174a46084aa9ac57fb25541de26618a54e940583b9f002c51c774d1a8f62`
**Build:** ESP-IDF v5.5.4 · ESP32-S3 · app `orbit.bin` 1,307,536 B (38% of the 2 MB OTA slot free)
**Note:** this is a **verbose diagnostic build** — log level DEBUG, so the boot log
prints `calling init function: 0x… on core: N` for every IDF startup init function.
That trace pinpoints any boot hang (see the table below). Once the boot is confirmed
working, rebuild with `CONFIG_LOG_DEFAULT_LEVEL_INFO` for a quiet firmware.

### v1.2 — boot-trace build (after v1.1 still stopped right after the PSRAM pool add)

- **Secondary console (USB-Serial-JTAG) removed** — UART0 only. This also drops
  the whole `esp_driver_usb_serial_jtag` component from the startup path
  (`init_vfs_usj_sec` ran at CORE-112, immediately after the PSRAM pool add).
- **DEBUG log level** — the init-function trace above.

### Init-function trace decoder (boot order)

If the boot log stops, the last `calling init function: 0x…` line names the culprit:

| Address | Stage | Function |
|---|---|---|
| 0x420035f8 | CORE | init_efuse_check |
| 0x42003a14 | CORE | init_show_cpu_freq |
| 0x42003518 | CORE | init_show_app_info |
| 0x4200360c | CORE | init_efuse_show_app_info |
| 0x420053d4 | CORE | init_heap |
| 0x42008b6c | CORE | esp_timer_init_nonos |
| 0x42008328 | CORE | init_libc |
| 0x42008fd4 | CORE | **add_psram_to_heap** ← v1.1 stopped right after this one |
| 0x42003a5c | CORE | init_brownout |
| 0x42003a6c | CORE | init_newlib_time |
| 0x42009db8 | CORE | init_vfs_uart |
| 0x4200bd04 | CORE | init_vfs_nullfs |
| 0x4200a8a0 | CORE | init_vfs_console |
| 0x42008334 | CORE | init_libc_stdio |
| 0x42003aac | CORE | init_flash |
| 0x420c794c | CORE | init_efuse |
| 0x420a07f8 | SECONDARY | esp_timer_init_os |
| 0x420c7a9c | SECONDARY | esp_security_init |
| 0x420060f8 | SECONDARY | esp_sleep_startup_init |
| 0x42003a78 | SECONDARY | init_apb_dma |
| 0x42003a84 | SECONDARY | **init_disable_rtc_wdt** ← ROM RTC watchdog is only disabled here |

(Generated from `firmware/build/orbit.elf`; CORE entries run first, then SECONDARY.)

## v1.1 — hardware bring-up fixes (after the first flash showed a dead boot loop)

- **Main task stack 3584 → 8192 B** — `app_main` runs the whole init chain on it;
  3.5 KB silently overflowed and hung the boot before the first log line (RTCWDT
  reset loop, no app output). Compiler stack-check (`-fstack-check`) now ON too.
- **Mic task: 32 KB PSRAM stack** (was 8 KB) — esp-sr MultiNet `detect()` needs
  ~28 KB; the task is created once and gated on/off (no per-conversation churn).
- **No more boot aborts**: every peripheral init (board, assets, audio, display,
  WiFi, alarms, MCP) is logged and non-fatal — a missing OLED/mic can no longer
  brick the boot. Only NVS stays fatal.
- **Boot beep** (880 Hz + 1320 Hz) after audio init — instant speaker feedback.
- **OLED driver**: PAGE addressing (works on SSD1306 *and* SH1106) + probes both
  I2C addresses 0x3C/0x3D; screen cleared on init.
- **Assets reader**: auto-detects the raw esp-sr `srmodels.bin` (was mis-parsed
  as a xiaozhi pack → garbage table log).
- **Music stream task 32 KB PSRAM** (was 6 KB — minimp3 needs ~17 KB scratch);
  MCP parallel tool tasks 20 KB PSRAM (TLS handshakes); Gemini tool task 6 KB;
  Gemini websocket task 12 KB (runs the WSS/TLS handshake).
## Hardware self-test (hold BOOT at power-on, ~20 s)

Power on (or reset) **while holding the BOOT button** — before the normal boot:

1. **Speaker**: three tones (440 / 880 / 1320 Hz).
2. **OLED**: 4 patterns, 2 s each — blank → border + `HW TEST` → checkerboard → full.
3. **Mic**: 6 s loopback — on-screen `MIC` bar + LED follow your voice; the serial
   log prints `🔧 mic level …` every 500 ms.

Then it continues the normal boot. If a stage fails, the serial log says which one.

## Flash

```bash
# erase once when coming from the old (16 MB-partitioned) firmware
esptool.py --chip esp32s3 -b 921600 erase_flash
# then flash the deliverable (from the project root)
esptool.py --chip esp32s3 -b 921600 write_flash 0x0 orbit_firmware.bin
# monitor
idf.py -p /dev/ttyACM0 monitor        # 115200 baud
```

Individual parts live in this directory (`bootloader.bin`, `partition-table.bin`,
`ota_data_initial.bin`, `orbit.bin` = the OTA payload for the setup portal);
the wake-word model image is `firmware/prebuilt/assets.bin` (flash at `0x420000`).
A full 16 MB image is not shipped (too big, slow uploads) — regenerate on demand:
`python3 tools/make_full_image.py firmware/release/orbit_full_flash.bin --full`

## First boot

1. Orbit starts the open AP **`Orbit-Setup`** (no WiFi saved yet).
2. Join it and open **http://192.168.4.1** → enter WiFi SSID/password, **Gemini API key**,
   Jamendo client ID (music; free at jamendo.com/api) and your city → *Save & reboot*.
3. Orbit joins your WiFi, syncs time (SNTP), shows the clock + `STANDBY` on the OLED.
4. Say **"Orbit"** (or press **BOOT**) → `LISTENING` → talk.

## What's inside

| Feature | Status |
|---|---|
| Wake word "Orbit" (ESP-SR MultiNet6 `mn6_en`, pre-trained, on-device) | ✅ |
| Gemini Live conversation (voice in/out, `gemini-3.8-live`, voice Kore) | ✅ |
| 15 local MCP tools (music, alarms, timers, volume, weather, news, time, city) | ✅ |
| Remote MCP servers over Streamable HTTP | ✅ |
| Music: Jamendo (full songs) · Deezer (30 s previews) · direct URLs | ✅ |
| Alarms & timers: NVS-persisted, SNTP-accurate, 15-min grace, snooze/stop | ✅ |
| 0.91" OLED UI (ORBIT logo · time · status · voice-reactive orb) | ✅ |
| One wake button (BOOT, GPIO0) — volume & everything else by voice | ✅ |
| WiFi setup portal (AP "Orbit-Setup" → http://192.168.4.1) + OTA upload | ✅ |
| Deep sleep after 5 min idle (RTC alarm wake + BOOT button wake) | ✅ |
| No WS2812 lamp (GPIO18 unused) · no Chinese assets · Gemini key only in NVS | ✅ |

## Hardware test checklist

- [ ] Boots: serial log `🛰️ Orbit assistant starting…` → `✅ Orbit is ready`
- [ ] OLED: ORBIT logo + time + status word; orb reacts to your voice
- [ ] Wake word: say "Orbit" → `🎤 WAKE WORD detected` → `🗣️ conversation open`
- [ ] Conversation: "what time is it?" → Orbit answers out of the speaker
- [ ] Barge-in: talk over Orbit → it stops and listens
- [ ] Button: BOOT while idle → wakes; while talking → ends the conversation
- [ ] Music: "play <song>" → Jamendo streams (needs the Jamendo client ID)
- [ ] Alarm: "set an alarm for 07:30" → survives reboot; rings on time (speaker + OLED `ALARM`)
- [ ] Snooze/stop: "snooze" / "stop alarm", or press BOOT while ringing
- [ ] Timer: "timer for 1 minute" → rings after 60 s
- [ ] Weather/news: "what's the weather?" / "news about space"
- [ ] Volume: "volume 80"
- [ ] Deep sleep: idle 5 min → `😴 … deep sleep`; press BOOT to wake
- [ ] RTC alarm wake: set an alarm, let it deep-sleep → wakes and rings on time
- [ ] OTA: on the portal, upload `firmware/release/orbit.bin` → reboots into it

## Known limits (v1)

- Music needs a free Jamendo client ID (full songs) — otherwise Deezer 30 s previews.
- Alarms use device-local time **IST (UTC+5:30)** — change `ORBIT_DEFAULT_TZ` in
  `components/alarm_timer/alarm_timer.c` if you move.
- The Gemini key is stored in NVS (never in the firmware image).
- Remote MCP tools need a reachable MCP server (Streamable HTTP, HTTPS).

## Rebuild everything

```bash
cd /home/user/orbit-assistant && bash tools/build_release.sh
#   self-heals the build env, rebuilds, regenerates orbit_firmware.bin at the root
```

## Source layout

```
firmware/
  main/app_main.c          state machine (idle → conversation → deep sleep)
  components/
    orbit_board            pins (mic/speaker I2S, BOOT button, OLED, LED)
    wake_word              ESP-SR mn6_en "Orbit" detection
    gemini_client          Gemini Live WebSocket client (audio + tool calls)
    music_player           Jamendo/Deezer/URL → minimp3 → 24 kHz speaker
    alarm_timer            NVS alarms/timers, SNTP, grace window, snooze
    wifi_config            STA + "Orbit-Setup" AP portal + OTA + SNTP
    audio_pipeline         I2S mic (16 kHz) + speaker (24 kHz), volume
    orbit_display          0.91" SSD1306 UI (logo/time/status/orb)
    settings               NVS settings (WiFi, Gemini key, Jamendo, city)
    live_info              weather (Open-Meteo), news (RSS), SNTP time
    mcp_common/local/client/router   MCP tool pipeline
    minimp3                vendored MP3 decoder (lieff/minimp3, CC0)
  partitions.csv           nvs/otadata/phy_init/ota_0/ota_1/assets (compact)
  prebuilt/assets.bin      raw esp-sr srmodels.bin (mn6_en "Orbit")
tools/                     build_assets.py, make_full_image.py, build_release.sh,
                           ensure_idf.sh (self-healing build env), backup/restore_sources.sh
orbit-sources.tar.gz       single-file backup of all sources (survives snapshots)
```
