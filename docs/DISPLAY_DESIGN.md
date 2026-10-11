# 🛰️ Orbit Display Design — 0.91" OLED (SSD1306, 128×32)

Status: **DESIGN PLAN — not yet implemented** (per user request: plan first, build later)
Target: Phase 2 (board bring-up)

---

## 1. Hardware change

| | Before (backup fw) | Now |
|---|---|---|
| Panel | 128×64 OLED (1.3") | **0.91" OLED, 128×32** |
| Controller | SSD1306 / SH1106 | **SSD1306** |
| Interface | I2C0, addr 0x3C, 400 kHz | same (drop-in swap) |
| Pins | SDA=GPIO41, SCL=GPIO42 | **same — no wiring change** |

Board config updates: `ORBIT_DISPLAY_WIDTH 128`, `ORBIT_DISPLAY_HEIGHT 32`.
Monochrome 1-bit panel — the whole UI is designed for 1-bit with contrast-based dimming.

---

## 2. Screen layout (128 × 32 px)

```
 x:   0         10        20        30        40        50        60        70        80      90       100      110      120
      0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567
 y=0 ┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
      │  ┌─ LEFT: TEXT (x 3..82) ─┐                                  ┌─ RIGHT: ANIMATION (x 86..126) ─┐                  │
 y=1 │  │  ORBIT                 │                                  │                                │                  │
      │  │  (bold logo, y 1..10)  │                                  │          ╭───────────╮         │                  │
 y=2 │  │                        │                                  │         │   ◜─────◝   │        │                  │
      │  ├────────────────────────┤                                  │        │  ◜       ◝  │        │   orb ring       │
 y=13│  │  12:34                 │                                  │        │  ◝       ◞  │        │   (voice-        │
      │  │  (6×8 font, y 13..20)  │                                  │        │   ◝─────◞   │        │    sensitive)    │
 y=14│  ├────────────────────────┤                                  │         ╰───────────╯         │                  │
      │  │  STANDBY               │                                  │                                │                  │
 y=23│  │  (6×8 font, y 23..30)  │                                  │   PLAYING state:               │                  │
      │  └────────────────────────┘                                  │   rotating disk + ♪ note       │                  │
 y=31└────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
      ◀────────── 80 px text block ──────────▶                  ◀──────── 41 px animation ────────▶
```

Pixel zones:

| Zone | x | y | Content |
|---|---|---|---|
| Brand logo | 3..48 | 1..10 | **ORBIT** — hand-crafted bold bitmap (~46×10 px), static |
| Time | 3..33 | 13..20 | `HH:MM` 24 h, 6×8 font; `--:--` until time is synced |
| Status | 3..75 | 23..30 | one uppercase word, 6×8 font (max ~10 chars) |
| Animation | 86..126 | 1..30 | orb ring (r=14, center 106,16) **or** rotating disk + ♪ |

Status words: `STANDBY` · `LISTENING` · `THINKING` · `SPEAKING` · `PLAYING` · `ALARM` · `OFFLINE`

---

## 3. Typography

- **Logo**: hand-crafted bold glyphs for O-R-B-I-T (10 px cap height, ~8–9 px per glyph, 1 px tracking) → ~46 px wide. This is the brand mark; drawn once, never animated. Bold = filled stems, 2 px stroke weight.
- **Text font**: compact 6×8 monospace bitmap (A–Z, 0–9, `: - . /`, space) — ~45 glyphs × 6 bytes. Crisp at 1×, no scaling artifacts.
- **Music note ♪**: hand-crafted 6×10 px eighth-note glyph (elliptical head + stem + flag), drawn inverted (knocked out) on the disk.

---

## 4. The Orb — voice-sensitive ring (right side)

A ring that **reacts to voice/audio level** (smoothed RMS, 0..1, fed from the audio pipeline):

| Element | Behaviour |
|---|---|
| Outer ring | static 1 px circle, r=14 |
| Main arc | 2 px thick arc at r=10–11, sweep = **70° + level×130°**, rotation = **60°/s + level×300°/s** |
| Counter arc | small 30° arc at r=6, counter-rotating, appears when level > 0.15 |
| Breathing | ring radius 14↔15 at ~2 Hz, depth ∝ level (subtle) |

Per state:

| State | Orb behaviour |
|---|---|
| `STANDBY` | level=0 → slow 60°/s arc, no pulse, **dimmed** (contrast 0x40) |
| `LISTENING` | mic level → fast arc, wide sweep, breathing pulse (**voice-sensitive**) |
| `SPEAKING` | TTS output level → fast arc + **two opposing arcs** |
| `THINKING` | rotating **dashed** ring (processing feel) |
| `ALARM` | ring blinks at 1 Hz + fast arc |
| `OFFLINE` | dim, very slow arc |
| `PLAYING` | orb **replaced** by the rotating disk (below) |

## 5. Rotating disk + music note (PLAYING state)

- **Filled disk**, r=14 (solid white).
- **Music note knocked out** (clear pixels) in the centre — the ♪ glyph from §3.
- **Rotation cue**: a 3 px-wide notch in the rim + a 1 px radial tick (r=11→13), both rotating at ~1 rev/s.
- Optional vinyl groove: 1 px knocked-out circle at r=8 — only if it stays clean at 28 px (verify on hardware).
- When music stops → back to the orb ring.

## 6. Rendering engine (the "polished" part)

- **Framebuffer**: 512 B (128×32, 1 bpp, page-major — matches SSD1306 layout → direct push, no conversion).
- **Primitives**: pixel set/clear, hline, vline, Bresenham line, midpoint circle, filled circle, parametric arc (1° steps → smooth sub-pixel motion), glyph blit (normal + inverted/knockout).
- **Dirty-region updates**: text zone (cols 0..83) redrawn **only when text changes**; animation zone (cols 86..126) redrawn every frame. Windowed SSD1306 writes: 176 B/frame @ 400 kHz I2C ≈ **0.5 ms** — no flicker, no bus saturation.
- **Frame rate**: **25 FPS** (40 ms) FreeRTOS task. 25 fps is smooth for rotation and cheap on CPU.
- **Monochrome polish rules**: 1 px grid, ≥3 px margins, stroke weights only 1–2 px, dimming via SSD1306 contrast (never pixel dithering), single accent structure (the ring/disk) so the eye lands in one place.

## 7. Driver

Custom minimal **SSD1306 I2C driver** (~200 lines): init sequence for 128×32 (multiplex 0x1F, COM pins 0x02, charge pump, contrast), windowed framebuffer push. **No external component needed** — avoids the component-registry dependency entirely.

## 8. Component API (`orbit_display`)

```c
typedef enum {
    ORBIT_DISPLAY_STANDBY = 0,
    ORBIT_DISPLAY_LISTENING,
    ORBIT_DISPLAY_THINKING,
    ORBIT_DISPLAY_SPEAKING,
    ORBIT_DISPLAY_PLAYING,   // music → rotating disk + ♪
    ORBIT_DISPLAY_ALARM,
    ORBIT_DISPLAY_OFFLINE,
} orbit_display_state_t;

esp_err_t orbit_display_init(void);                    // I2C + SSD1306 128x32 + render task
void      orbit_display_set_state(orbit_display_state_t state);
void      orbit_display_set_time(int hour, int minute);   // 24 h; -1,-1 → "--:--"
void      orbit_display_set_audio_level(float level01);   // voice sensitivity, 0..1
```

## 9. Integration map

| Source | Feeds |
|---|---|
| `audio_pipeline` (mic/TTS RMS) | `set_audio_level` → orb speed/sweep/pulse |
| app state machine (`main`) | `set_state` |
| `music_player` | `PLAYING` (disk + ♪) |
| `alarm_timer` | `ALARM` |
| `gemini_client` | `LISTENING` / `THINKING` / `SPEAKING` |
| SNTP time sync (Phase 4/7) | `set_time` (refresh each minute) |

## 10. Phase 2 hardware test (on the 0.91" panel)

1. Display test: logo, time, then **cycle through all 7 states** with the orb animating; PLAYING shows the spinning disk + ♪.
2. **Voice sensitivity proof**: speak into the mic while in LISTENING → arc speeds up, widens, and pulses with your voice.
3. Audio: mic→speaker loopback + test tone.
4. Buttons: all 4 events logged.
5. Verify on hardware: logo/time/status legibility, animation smoothness, zero I2C errors.

## 11. Polish checklist

- [ ] consistent 1 px grid, ≥3 px margins everywhere
- [ ] stroke weights only 1–2 px
- [ ] no flicker — text redraws only on change, animation zone is small
- [ ] dim states via contrast, not dithering
- [ ] smooth motion — 25 fps, 1° arc steps
- [ ] legible at a glance — big brand, clear time, one-word status
