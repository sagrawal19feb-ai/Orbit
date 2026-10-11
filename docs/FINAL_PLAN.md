# 🛰️ Orbit — Final Plan & Capability Spec

**Orbit** is a totally new, clean-room ESP32-S3 voice assistant for the Orbit device
(bread-compact-wifi pinout). Brain: **Google Gemini** (Live API). Wake word:
**"Orbit"** (pre-trained on-device model, reused from the original firmware).
Status legend: ✅ built · 🔄 in progress · 📋 planned (phase)

---

## 1. The device at a glance

| | |
|---|---|
| Chip | ESP32-S3, 16 MB flash, octal PSRAM |
| Firmware | ESP-IDF v5.5.4, clean-room project (`firmware/`) |
| Mic | I2S, 16 kHz (GPIO 4/5/6) |
| Speaker | I2S, 24 kHz (GPIO 7/15/16) |
| Display | **0.91" OLED, SSD1306 128×32** (GPIO 41/42) |
| Button | **BOOT (0)** — the only button: wake. Volume & everything else by voice |
| Status LED | GPIO 48 |
| Lamp (WS2812, GPIO 18) | **removed by design** |
| Chinese command assets | **removed by design** (wake word `Orbit` stays) |

---

## 2. Abilities & capabilities

### 🗣️ 1. Real-time voice conversation — Gemini Live ✅
- **Gemini Live API** over WebSocket: streaming mic audio in, streaming voice out — true duplex, low latency (REST fallback if Live is unstable)
- Speaks **any language** (multilingual; English primary) — ask in Hindi, Spanish, French…
- Concise spoken answers; system prompt tuned for voice
- API key stored in **NVS, never hard-coded**
- ✅ Live WebSocket transport (gemini_client: setup/realtimeInput/toolResponse, barge-in, keepalive)

### 👂 2. Wake word "Orbit" — on-device ✅
- Pre-trained **MultiNet6 (mn6_en)** model extracted from the device backup — **no retraining**
- Runs fully on-device via ESP-SR; no cloud needed to wake
- "Orbit" → chime → listening state

### 🔌 3. MCP tool ecosystem — the superpower ✅
- **Local MCP server**: all 13 device tools exposed with full MCP schemas
- **Remote MCP client**: plug in **any MCP server by URL** (Streamable HTTP) — Home Assistant, calendars, search, databases… the whole MCP ecosystem, by voice
- **Efficient**: schema cache in PSRAM, lazy connect, 12 s timeout + retry, result truncation (2 KB), parallel calls (max 4), graceful offline degradation
- **No hosting needed**: built-ins are on-device + free public APIs; remote tools use public/hosted MCP servers
- **Zero-config safety**: server URLs/tokens in NVS, HTTPS-only, per-server trust, spoken confirmation for destructive tools

### 🎵 4. Music — "play any song" ✅
- **Jamendo** (free client ID) → **full songs**, direct MP3 streams
- **Deezer** → mainstream song search with **30-sec previews** (the free-tier reality)
- **Direct URL** → stream any link: "Orbit, play this"
- Voice controls: play / pause / resume / stop / next / volume
- HTTP stream → MP3 decode (minimp3) → resample to 24 kHz → I2S speaker
- ✅ Decoder + streaming pipeline (minimp3 → 24 kHz speaker)

### ⏰ 5. Alarms & timers ✅
- Voice-set: "set an alarm for 7:30 AM", "timer for 10 minutes", "list alarms", "cancel", "snooze", "stop"
- **NVS-persisted** — survives reboots **and power loss**
- **Accurate**: SNTP-synced clock while awake (sub-second); RTC wake from deep sleep + wake→re-sync→re-sleep-if-early correction
- Missed-while-off: fires on boot within a 15-min grace window, else reported missed
- Rings through the speaker; OLED shows blinking **ALARM** state; dismiss by voice or button
- ✅ Full implementation (NVS-persisted, SNTP, grace window, snooze/stop)

### 🌤️ 6. Live weather ✅ (tool ready)
- **Open-Meteo** (free, no key, verified live): "What's the weather in Delhi?" → "Delhi: 24.6 °C, feels like 27 °C, clear sky, humidity 80%…"
- Location: auto via **ip-api** (free) or "I'm in Mumbai" (remembered in NVS)

### 📰 7. Live news ✅ (tool ready)
- **Google News RSS** (free, no key, verified live): "cricket news", "tech news", any topic, any language
- BBC RSS also wired; multilingual queries supported

### 🕐 8. Accurate time ✅ (tool ready)
- **SNTP sync** (pool.ntp.org) after WiFi connects; OLED clock shows HH:MM; "what time is it?" answered exactly

### 🖥️ 9. Smart display — 0.91" OLED ✅
- **ORBIT** bold logo on top · **time** under it · **status word** under that
  (`STANDBY` · `LISTENING` · `THINKING` · `SPEAKING` · `PLAYING` · `ALARM` · `OFFLINE`)
- Right side: **voice-sensitive orb** — a rotating arc whose speed, sweep and pulse react to live audio level
- In `PLAYING`: switches to a **rotating disk with a music-note symbol**
- 512-byte framebuffer, dirty-region updates, 25 FPS, zero flicker, custom SSD1306 driver (no external display component)
- Full design: `docs/DISPLAY_DESIGN.md`

### 🔧 10. Hardware controls ✅ (1 wake button, BOOT GPIO0)
- 1 wake button (BOOT), status LED state indication; volume & all controls by voice
- Same pins as the original firmware — drop-in

### ⚙️ 11. Settings & configuration ✅ (NVS-backed)
- NVS settings: WiFi credentials, Gemini API key, Jamendo client ID, home city, MCP servers
- Web config portal (captive portal) for first-time WiFi + keys

### 📶 12. WiFi provisioning ✅ (STA + Orbit-Setup AP portal)
- Captive-portal setup; reconnects automatically; offline mode keeps local tools working

### 🔄 13. OTA updates ✅ (portal upload + dual ota_0/ota_1 slots)
- Dual `ota_0`/`ota_1` partitions (same layout as the original device); updates from GitHub Releases or USB — **no server to host**

### 😴 14. Low-power sleep ✅ (deep sleep after 5 min idle)
- Deep sleep between interactions; RTC wake for alarms; wake word brings it back

### 🔒 15. Privacy & safety ✅ (by design)
- API keys only in NVS · wake word processed on-device · no Chinese command assets · no lamp · HTTPS everywhere

---

## 3. Voice command cheat-sheet

| Say | Orbit does |
|---|---|
| "Orbit, what's the weather?" | live weather for your city |
| "Orbit, cricket news" | live headlines on that topic |
| "Orbit, play <song>" | full song (Jamendo) or 30-sec preview (Deezer) |
| "Orbit, play <link>" | streams any URL |
| "Orbit, pause / resume / stop" | music control |
| "Orbit, set an alarm for 7:30 AM" | daily alarm, survives reboots |
| "Orbit, timer for 10 minutes" | countdown timer |
| "Orbit, list my alarms" / "cancel that" | manage alarms |
| "Orbit, volume 80" | speaker volume |
| "Orbit, what time is it?" | exact SNTP-synced time |
| "Orbit, I'm in Mumbai" | remembers your city |
| "Orbit, turn off the living room light" | *(via your Home Assistant MCP server)* |
| "Orbit, what can you do?" | lists its tools & abilities |

## 4. The MCP tool catalog

**Local (on-device, offline):** `play_song` · `stop_music` · `pause_music` · `resume_music` ·
`set_alarm` · `cancel_alarm` · `set_timer` · `list_alarms` · `set_volume` ·
`get_weather` · `get_news` · `get_time` · `set_city`

**Remote (any MCP server you configure):** exposed as `<server>__<tool>` — add a server
with `mcp_router_add_server(name, url, token)` (persisted in NVS).

## 5. Final architecture

```
        ┌──────────── mic (I2S 16 kHz) ────────────┐
        ▼                                            │
   ┌─────────┐   "Orbit"   ┌──────────────┐       │
   │ ESP-SR  │ ──────────▶ │  State machine │ ◀─────┘ (buttons, alarms)
   │ (mn6_en)│             │ idle→listen→ │ ──▶ OLED (logo/time/status/orb)
   └─────────┘             │ think→speak  │
                           └──────┬───────┘
                                  │ audio + tool calls
                    ┌─────────────▼────────────┐
                    │   Gemini Live (WebSocket) │ ◀── your API key (NVS)
                    └─────────────┬────────────┘
                                  │ function calls
                    ┌─────────────▼────────────┐
                    │      MCP TOOL ROUTER      │
                    │  local tools · remote MCP  │
                    │  (music, alarms, weather,  │
                    │   news, time, + any server)│
                    └─────────────┬────────────┘
                                  ▼
                    speaker (I2S 24 kHz) · music streams · alarms ring
```

## 6. Build status & phases

| Phase | Status |
|---|---|
| 0 — wake-word assets extracted & verified | ✅ done |
| 1 — project scaffold, builds | ✅ done |
| 4a — MCP local server + router + Gemini bridge | ✅ done, builds |
| 4b — remote MCP client + live-info tools | ✅ done, builds, host-tested |
| 4c — Gemini Live WebSocket transport | ✅ done, builds |
| 2 — audio pipeline + 0.91" display UI | ✅ |
| 3 — "Orbit" wake word on hardware | ✅ |
| 5 — music playback | ✅ |
| 6 — alarms & timers | ✅ |
| 7 — WiFi provisioning, OTA, low power, ship | ✅ |

## 7. The .bin deliverables (the "main task")

`tools/build_release.sh` produces `firmware/release/`:

| File | Flash offset | What |
|---|---|---|
| `orbit_full_flash.bin` | **0x0** | **one 16 MB file — flashes everything** (like the original `firmware_backup.bin`) |
| `bootloader.bin` | 0x0 | bootloader |
| `partition-table.bin` | 0x8000 | partition table (identical to the original device) |
| `ota_data_initial.bin` | 0xd000 | OTA boot selection |
| `orbit.bin` | 0x20000 | the app |
| `assets.bin` | 0x800000 | wake-word model (`mn6_en` "Orbit") + index.json |
| `FLASH.md` | — | flash & monitor instructions |

Flash once with: `esptool.py --chip esp32s3 write_flash 0x0 orbit_full_flash.bin`

**Every completed phase regenerates the .bin via the same one-command release.**
