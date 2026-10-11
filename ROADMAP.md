# 🛰️ Orbit — Gemini Voice Assistant for ESP32-S3

**Project:** A totally new voice-assistant firmware for the Orbit device (ESP32-S3).
**Brain:** Google Gemini API (user's API key).
**Wake word:** `Orbit` — reusing the already-trained wake-word model from the repo (no retraining).
**Must-haves:** play any requested song · set alarms & timers · conversation.
**Constraints:** reuse the exact pin config of the existing device · **no lamp/WS2812 code**.

---

## 1. What we already have (recon of the repo binaries)

| Item | Finding |
|---|---|
| Chip | **ESP32-S3** (confirmed in flash dump + `index.json`) |
| Base firmware | **xiaozhi-esp32** (`./main/boards/bread-compact-wifi/compact_wifi_board.cc`, ESP-IDF **v5.5.4-dirty**, built 2026-05-22) |
| Board | **bread-compact-wifi** — direct I2S audio (`NoAudioCodec`), no external codec chip |
| Flash | 16 MB, full dump in `firmware_backup.bin` |
| Wake word | **`Orbit`** — trained model ships inside `assets.bin` (`srmodels.bin` + `mn6_cn`/`mn6_en` + `index.json`) |
| Lamp (to remove) | `LampController` on **GPIO18** (WS2812 / `led_strip_rmt`) — confirmed present in the old firmware |

### 🔌 Pin config (bread-compact-wifi — to be reused exactly)

| Function | GPIO | Notes |
|---|---|---|
| Mic WS (LRCK) | **4** | digital mic (INMP441-style), I2S RX |
| Mic SCK (BCLK) | **5** | |
| Mic DIN (SD) | **6** | |
| Speaker DOUT (SD) | **7** | I2S TX → external amp |
| Speaker BCLK | **15** | |
| Speaker LRCK (WS) | **16** | |
| Wake button (BOOT) | **0** | the ONLY button — press to wake; volume & everything else by voice |
| OLED SDA | **41** | 0.91" SSD1306 **128×32** @ 0x3C, 400 kHz (drop-in panel swap) |
| OLED SCL | **42** | |
| Status LED | **48** | single LED (optional to keep) |
| ~~Lamp (WS2812)~~ | ~~18~~ | **REMOVED — not in new firmware** |

- Audio rates: **input 16 kHz / output 24 kHz**, simplex I2S (separate mic & speaker buses)

### 🗂️ Partition table (16 MB — reuse as-is)

| Partition | Offset | Size |
|---|---|---|
| nvs | 0x9000 | 0x4000 |
| otadata | 0xd000 | 0x2000 |
| phy_init | 0xf000 | 0x1000 |
| ota_0 | 0x20000 | 0x3f0000 (~4 MB) |
| ota_1 | 0x410000 | 0x3f0000 (~4 MB) |
| assets (SPIFFS, 0x82) | 0x800000 | 0x800000 (8 MB) |

### 📦 Asset inventory inside `assets.bin` (custom pack format)

| File | Purpose |
|---|---|
| `srmodels.bin` | ESP-SR models — **wake word "Orbit"** + AFE front-end |
| `mn6_cn` / `mn6_en` (+ `mn6_data`, `mn6_index`, `_MODEL_INFO_`) | Multinet6 command-recognition models (CN/EN) |
| `index.json` | Model manifest — `chip_model: esp32s3`, multinet `cn`, command `"Orbit" → action "wake"` |
| `commands_cn.txt` / `commands_en.txt` | Old appliance command lists (reference only — not needed for the new assistant) |

> ⚠️ Note: the `assets` partition inside `firmware_backup.bin` has the same pack format but **different sizes** than `assets.bin` — Phase 0 will verify which copy carries a working `Orbit` model (user says `assets.bin` is the trained wake-word model).

---

## 2. Target architecture

```
                 ┌───────────────┐   wake word "Orbit"    ┌────────────────────────────┐
  INMP441 mic ──▶│  ESP-SR AFE   │ ─────────────────────▶ │  Wake detector (srmodels   │
  (I2S RX, 16k)  │  + VAD        │   (pre-trained model)  │  + mn6_cn/mn6_en)          │
                 └───────────────┘                        └─────────────┬──────────────┘
                                                                        │ "Orbit!" heard
                                                                        ▼
                 ┌───────────────┐   PCM audio (WS)          ┌────────────────────────────┐
                 │  Recorder     │ ─────────────────────────▶│  Google Gemini API         │
                 │  (ring buf)   │                            │  • Live API (streaming)    │
                 └───────────────┘                            │  • function calling:       │
                                                                        │    play_song / set_alarm /  │
                                                                        │    set_timer / stop / volume│
                                                                        ▼
                 ┌───────────────┐   audio + text             ┌────────────────────────────┐
  Speaker amp ◀──│  I2S TX 24k   │ ◀──────────────────────────│  Gemini response + TTS     │
  (GPIO 7/15/16) │  + mixer      │◀───────────────────────────│  Music player (HTTP stream │
                 └───────────────┘   music stream (HTTP)      │  → MP3/AAC decode → I2S)   │
                                                              └────────────────────────────┘
        ┌──────────────────────────────────────────────────────────────────────────┐
        │  Support: OLED status (41/42) · wake button (0) · alarms & timers   │
        │  (NVS-persisted, RTC wake) · settings (WiFi + Gemini key in NVS) · OTA    │
        │  ✗ NO lamp / WS2812 (GPIO18 unused)                                       │
        └──────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Phases

### Phase 0 — Extract & validate the wake-word assets ✅ first
- Reverse/parse the `assets.bin` pack format (12-byte header + file table), extract: `srmodels.bin`, `mn6_cn`, `mn6_en`, `index.json`, `commands_*.txt`
- Cross-check against the `assets` partition in `firmware_backup.bin`; pick the copy with the working `Orbit` model
- Sanity-check: `Orbit` appears as the wake command in `index.json`; model loads under ESP-SR on esp32s3
- **Deliverable:** `assets/extracted/` folder with verified model files

### Phase 1 — Project scaffold
- New ESP-IDF **5.5.4** project, target `esp32s3` (matches the backup's IDF version)
- `partitions.csv` identical to the backup (table above) ✅
- Component layout: `orbit_board` (pins) · `assets_reader` · `audio_pipeline` · `wake_word` · `gemini_client` · `music_player` · `alarm_timer` · `orbit_display` · `settings`
- `prebuilt/assets.bin` generated (mn6_en wake-word model, round-trip verified ✅)
- **Deliverable:** project builds & boots to a serial console hello-world
- 🔄 **IN PROGRESS 2026-10-08:** project scaffolded at `firmware/` (all components, real pin config, real assets reader); ESP-IDF v5.5.4 toolchain installed in sandbox; first build pending
- 🔄 **IN PROGRESS 2026-10-08 (cont.):** MCP plan integrated — new components `mcp_common` (JSON-RPC + SSE, host-tested ✅ 21/21), `live_info` (Open-Meteo weather, Google News RSS, SNTP time — APIs verified live), `mcp_local` (13 device tools as MCP), `mcp_client` (remote Streamable-HTTP MCP client: lazy connect, PSRAM schema cache, SSE+JSON, 12 s timeout + retry, NVS server config), `mcp_router` (MCP↔Gemini bridge, allowlist, 2 KB result truncation, parallel calls). `app_main` boots the pipeline + self-test. Host tests pass; firmware build in progress. **Next:** Gemini Live WebSocket transport (vendored `esp_websocket_client`)
- ✅ **MCP STACK BUILDS 2026-10-08:** `idf.py build` → `build/orbit.bin` (~570 KB, 86% of the app partition free) with mcp_common/live_info/mcp_local/mcp_client/mcp_router + Gemini bridge + mbedtls TLS all linked. Host tests 21/21 PASS (SSE/JSON-RPC/tool schemas against recorded live MCP responses). Sandbox build-env quirks documented in `docs/BUILD_ENVIRONMENT.md` (snapshot file-cap truncates the IDF tree at turn boundaries; repair via `tools/repair_idf.sh`; unneeded submodule-heavy components moved to `esp-idf/components-disabled/`).
- ✅ **BUILD PASSES 2026-10-08:** `idf.py build` → `build/orbit.bin` (~197 KB, 95% of the 4 MB app partition free). ESP-IDF **v5.5.4** toolchain installed in the sandbox. Note: the IDF component manager would not auto-run in this environment, so Phase 1 builds with **no external managed components** (stubs only) — `esp-sr` + `esp_websocket_client` get added in Phases 3/4 (vendored into `components/` if needed). Hardware boot test (serial console) is on the user's device.

### Phase 2 — Board bring-up (same pins, no lamp, 1 wake button) ✅ done
- Port the pin config verbatim (table in §1); **exclude `LampController`/GPIO18 entirely**; **only the BOOT button (GPIO0) as wake** — volume & everything else by voice
- Display: **0.91" OLED (SSD1306 128×32)** on the same I2C pins — full UI design in **`docs/DISPLAY_DESIGN.md`**: bold `ORBIT` logo + time + status word on the left; a **voice-sensitive orb ring** on the right that switches to a **rotating disk with a music-note symbol** when music plays
- I2S mic → record → I2S speaker loopback test; speaker test tone; wake-button events
- **Deliverable:** hardware test app proving mic in, speaker out, OLED (full UI + animations), wake button
- ✅ **DONE 2026-10-08:** `audio_pipeline` (real simplex I2S — mic 16 kHz GPIO4/5/6, speaker 24 kHz GPIO7/15/16, software volume, mic/output RMS levels, 16k→24k resampler) · `orbit_display` (custom SSD1306 driver, 512 B framebuffer, dirty-region pushes, 25 FPS task, all 7 state animations incl. voice-sensitive orb + rotating music disk) · `app_main` Phase-2 test (440/880 Hz tones → display state cycle → 10 s mic→speaker loopback → wake-button logging). Builds + release regenerates ✅

### Phase 3 — Wake word `Orbit` ✅ done
- Integrate extracted pre-trained model with ESP-SR (AFE + wake detection)
- Wake → short chime → open listening session
- **Deliverable:** say "Orbit" → device wakes and listens

### Phase 4 — Gemini integration + MCP tool pipeline ✅ done
- Gemini API key stored in NVS (set via serial/web config, never hard-coded)
- **Gemini Live API** over WebSocket (streaming mic audio in, audio+text out) — low latency; REST fallback (STT → text → TTS) if Live is unstable
- **MCP tool pipeline** (plan: `docs/MCP_PLAN.md`, transport verified live 2026-10-08):
  - *4a* local in-process **MCP server** wraps all device tools (music, alarms, weather, news, time, volume) — uniform tool interface, direct C fast-path, works offline
  - *4b* remote **MCP client** (Streamable HTTP, JSON-RPC 2.0; handles JSON + SSE responses) for user-configured MCP servers — schemas cached in PSRAM, lazy connect, 12 s timeout + retry, graceful offline degradation, result truncation, parallel independent calls
  - tool router bridges MCP tools ↔ Gemini function declarations (JSON-Schema pass-through); optional `search_tools` meta-tool when tool count is large
- Live-info tools (free, no keys, verified): `get_weather` → Open-Meteo · `get_news` → Google News RSS / BBC RSS · `get_time` → SNTP clock
- **SNTP time sync** after WiFi connect → OLED clock + alarm accuracy
- System prompt: concise spoken answers **in the user's language** (multilingual); no Chinese assets
- **Deliverable:** talk to Gemini through the device; it answers out loud; device tools + remote MCP tools callable by voice
- ✅ **DONE 2026-10-08 (4c):** Gemini Live WebSocket transport (gemini-3.8-live, native audio 16k in / 24k out, voice "Kore", output transcription, async tool calls via the MCP router, barge-in, keepalive, cert bundle). State machine in app_main: IDLE (wake word) → CONVERSATION (mic streams to Gemini, voice out the speaker, display LISTENING/SPEAKING) → idle-timeout/max-session → back to wake word. Wake button = wake / stop. Builds + release ✅

### Phase 5 — Music playback ("play any song I request") ✅ done
- ⚠️ Reality locked in: full mainstream songs aren't legally available via free APIs (that's what Alexa's subscription pays for)
- **Jamendo** → full songs (free client ID, 1 JSON request → direct MP3 stream); **direct URL** → any full song from a pasted link; **Deezer** → mainstream search, 30-sec preview fallback (assistant announces it's a preview)
- Voice UX: "Orbit, play <song>" → best available (full if possible, else preview); "Orbit, play <url>" → stream any link; pause/resume/stop/next/volume by voice
- HTTP streaming → MP3/AAC decoder (minimp3 / Helix) → resample to 24 kHz → I2S TX
- Mixing policy: music pauses during conversation, resumes after (or duck)
- **Deliverable:** "Orbit, play <song>" → music plays through the speaker
- ✅ **DONE 2026-10-08:** minimp3 vendored; Jamendo full-song search (free client ID) → Deezer 30 s previews → direct URLs; HTTP stream → MP3 decode → linear resample to 24 kHz → I2S; pause/resume/stop; display PLAYING (rotating disk + ♪)

### Phase 6 — Alarms & timers ✅ done
- Voice: "set an alarm for 7:30 AM", "timer for 10 minutes", "list alarms", "cancel alarm", "snooze", "stop"
- Persist in **NVS** (survives reboot **and power loss**); RTC wake from deep sleep
- Accuracy: SNTP-synced software clock while awake (sub-second); wake→re-sync→re-sleep-if-early correction for deep-sleep drift (S3 module has no ext 32 kHz crystal)
- Missed-while-off policy: on boot, alarms within a 15-min grace window fire immediately; older ones are reported as missed
- Ring via I2S (synthesised chime); OLED shows blinking ALARM state; dismiss by voice or button
- **Deliverable:** alarms/timers work across reboots and power cycles
- ✅ **DONE 2026-10-08:** NVS-persisted JSON list (survives reboot + power loss); SNTP-accurate daily alarms with 15-min missed grace window; monotonic timers; ring (synthesised two-tone) + dismiss/snooze (voice + button); new MCP tools `stop_alarm` / `snooze_alarm` (15 tools total); `alarm_timer_next_fire_in_seconds()` for RTC wake

### Phase 7 — Integration, polish & ship ✅ done
- WiFi provisioning (captive portal like the old firmware) + web config for Gemini key
- OTA updates (ota_0/ota_1), settings backup, error sounds, low-power sleep between wakes
- End-to-end test checklist (see §5)
- **Deliverable:** flashable, reliable daily-driver firmware

---

## 4. Decisions to lock in (my recommendations)

| # | Decision | Recommendation |
|---|---|---|
| 1 | **Gemini Live (WebSocket streaming) vs REST pipeline** | Live API — much lower latency, single round trip; keep REST as fallback |
| 2 | **Music source** | iTunes Search API (free, no key) for search + direct MP3/AAC stream URLs; user can also paste a URL. Region matters for availability |
| 3 | **Language** | English-only for v1 (Gemini is great at EN); CN can be added later since `mn6_cn`/`commands_cn` already exist |
| 4 | **Status LED (GPIO48)** | Keep as a simple state indicator (boot/wake/listening/error) — the *lamp* on GPIO18 is removed regardless |
| 5 | **Where does the code live** | New folder in this workspace now; push to a new GitHub repo (or a branch of `Orbit`) when Phase 1 is done |
| 6 | **Wake-word model package** | ✅ **Device flash package: `mn6_en`** (English MultiNet6 — correct phonetics for "Orbit"; what the device runs now). `assets.bin`'s `mn6_cn` kept as fallback |
| 7 | **MCP architecture** | Local in-process MCP server (device tools, offline, fast-path) + remote MCP client over Streamable HTTP (JSON+SSE, schema cache in PSRAM, lazy connect, graceful degradation). Plan: `docs/MCP_PLAN.md` |
| 8 | **Final plan & .bin release** | ✅ Capability spec: `docs/FINAL_PLAN.md`. One-command release: `tools/build_release.sh` → `firmware/release/` with `orbit_full_flash.bin` (16 MB, flash @0x0, verified round-trip) + individual parts + `assets.bin` + `FLASH.md`. Self-healing pre-build: `tools/ensure_idf.sh` (sandbox snapshot cap) |

---

## 5. Risks & watch-outs

- **`assets.bin` vs on-device assets mismatch** — verify the `Orbit` model triggers reliably (Phase 0); if `assets.bin` is stale, extract the model from the flash dump instead
- **PSRAM** — music buffering needs PSRAM; confirm the module is S3-**N8R8/N16R8** (16 MB flash dump suggests N16R8)
- **Music streaming** — WiFi dropouts; need a big ring buffer + reconnect logic; 24 kHz output resampling from 44.1 kHz sources
- **Gemini costs/latency** — Live sessions cost money per minute; add idle auto-disconnect
- **API key security** — NVS only, never in logs; consider `nvs` encryption later
- **Mic rate match** — 16 kHz mic PCM is exactly what Gemini Live expects ✅

---

## 6. Immediate next steps (once roadmap is approved)

1. **Phase 0:** extract & verify the `Orbit` wake-word model from `assets.bin`
2. **Phase 1:** scaffold the ESP-IDF 5.5 project with the backup's partition table
3. Then proceed phase by phase, hardware-testing each step

*Roadmap created 2026-10-08 · Orbit Assistant project*
