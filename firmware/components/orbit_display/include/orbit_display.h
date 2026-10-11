#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Orbit display UI — 0.91" OLED (SSD1306 128x32).
 * Left: bold ORBIT logo · time · status word.
 * Right: voice-sensitive orb ring → rotating music-note disk when playing.
 * Design: docs/DISPLAY_DESIGN.md
 */

typedef enum {
    ORBIT_DISPLAY_STANDBY = 0,
    ORBIT_DISPLAY_LISTENING,
    ORBIT_DISPLAY_THINKING,
    ORBIT_DISPLAY_SPEAKING,
    ORBIT_DISPLAY_PLAYING,
    ORBIT_DISPLAY_ALARM,
    ORBIT_DISPLAY_OFFLINE,
} orbit_display_state_t;

esp_err_t orbit_display_init(void);
void orbit_display_set_state(orbit_display_state_t state);
void orbit_display_set_time(int hour, int minute); /* 24 h; -1,-1 → "--:--" */
void orbit_display_set_audio_level(float level01);  /* manual override (testing); negative = off */

/* hardware test (hold BOOT at power-on) */
void orbit_display_test_mode(bool on);          /* pause the render task */
void orbit_display_test_pattern(uint8_t phase); /* 0 clear · 1 border+HW TEST · 2 checker · 3 full · 4 mic bar */
