#include "orbit_display.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "orbit_board.h"
#include "audio_pipeline.h"
#include "ssd1306.h"
#include "font5x7.h"
#include "orbit_logo.h"

static const char *TAG = "display";

/* framebuffer: page-major 1bpp — matches the SSD1306 layout */
static uint8_t s_fb[SSD1306_PAGES][SSD1306_WIDTH];

/* zones */
#define TEXT_COL0 0
#define TEXT_COL1 84
#define ANIM_COL0 86
#define ANIM_COL1 126
#define ANIM_CX   106
#define ANIM_CY   16
#define ANIM_R    14

static orbit_display_state_t s_state = ORBIT_DISPLAY_STANDBY;
static int s_hour = -1, s_minute = -1;
static int s_last_minute = -2;
static bool s_text_dirty = true;
static bool s_test_mode = false; /* hardware test: render task paused */
static bool s_manual_level = false;
static float s_manual_level_val = 0.0f;
static float s_level = 0.0f; /* smoothed 0..1 */
static uint32_t s_frame = 0;
static uint8_t s_last_contrast = 0xFF;

/* ---------------- primitives ---------------- */

static void set_pixel(int x, int y, int color)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) {
        return;
    }
    uint8_t mask = (uint8_t)(1 << (y & 7));
    if (color) {
        s_fb[y >> 3][x] |= mask;
    } else {
        s_fb[y >> 3][x] &= (uint8_t)~mask;
    }
}

static void hline(int x0, int x1, int y)
{
    for (int x = x0; x <= x1; x++) set_pixel(x, y, 1);
}

static void draw_circle(int cx, int cy, int r)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        set_pixel(cx + x, cy + y, 1); set_pixel(cx - x, cy + y, 1);
        set_pixel(cx + x, cy - y, 1); set_pixel(cx - x, cy - y, 1);
        set_pixel(cx + y, cy + x, 1); set_pixel(cx - y, cy + x, 1);
        set_pixel(cx + y, cy - x, 1); set_pixel(cx - y, cy - x, 1);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

static void fill_circle(int cx, int cy, int r)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        hline(cx - dx, cx + dx, cy + dy);
    }
}

/* arc, 2 px thick (plots radii r and r+1), 0° = 3 o'clock, clockwise */
static void draw_arc(int cx, int cy, int r, float a0_deg, float sweep_deg)
{
    for (float a = a0_deg; a <= a0_deg + sweep_deg; a += 1.0f) {
        float rad = a * (float)M_PI / 180.0f;
        for (int rr = r; rr <= r + 1; rr++) {
            int x = cx + (int)lroundf(rr * cosf(rad));
            int y = cy + (int)lroundf(rr * sinf(rad));
            set_pixel(x, y, 1);
        }
    }
}

static void draw_char(int x, int y, char c)
{
    if (c < 32 || c > 126) c = '?';
    const unsigned char *g = ORBIT_FONT5X7[c - 32];
    for (int col = 0; col < 5; col++) {
        for (int bit = 0; bit < 7; bit++) {
            if (g[col] & (1 << bit)) {
                set_pixel(x + col, y + bit, 1);
            }
        }
    }
}

static void draw_text(int x, int y, const char *s)
{
    while (*s) {
        draw_char(x, y, *s++);
        x += 6;
    }
}

/* row-major bitmap glyph (msb = leftmost); invert=true clears the set pixels */
static void blit_glyph(int x, int y, const unsigned char *glyph, int w, int h, bool invert)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int bit = (glyph[row] >> (w - 1 - col)) & 1;
            set_pixel(x + col, y + row, invert ? !bit : bit);
        }
    }
}

/* ---------------- zones ---------------- */

static const char *status_word(orbit_display_state_t st)
{
    switch (st) {
    case ORBIT_DISPLAY_LISTENING: return "LISTENING";
    case ORBIT_DISPLAY_THINKING:  return "THINKING";
    case ORBIT_DISPLAY_SPEAKING:  return "SPEAKING";
    case ORBIT_DISPLAY_PLAYING:   return "PLAYING";
    case ORBIT_DISPLAY_ALARM:     return "ALARM";
    case ORBIT_DISPLAY_OFFLINE:   return "OFFLINE";
    default:                      return "STANDBY";
    }
}

static void clear_zone(uint8_t col0, uint8_t col1)
{
    for (int p = 0; p < SSD1306_PAGES; p++) {
        for (int c = col0; c <= col1; c++) {
            s_fb[p][c] = 0;
        }
    }
}

static void draw_text_zone(void)
{
    clear_zone(TEXT_COL0, TEXT_COL1);
    for (int i = 0; i < ORBIT_LOGO_GLYPHS; i++) {
        blit_glyph(3 + i * (ORBIT_LOGO_GLYPH_W + 1), 1,
                   ORBIT_LOGO[i], ORBIT_LOGO_GLYPH_W, ORBIT_LOGO_GLYPH_H, false);
    }
    char tbuf[16];
    if (s_hour < 0) {
        snprintf(tbuf, sizeof(tbuf), "--:--");
    } else {
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", s_hour, s_minute);
    }
    draw_text(3, 13, tbuf);
    draw_text(3, 23, status_word(s_state));
}

/* ---------------- animations ---------------- */

static void draw_orb(float t)
{
    int ring_r = ANIM_R + (int)(s_level * 1.5f); /* breathing */
    draw_circle(ANIM_CX, ANIM_CY, ring_r);

    float speed, sweep;
    switch (s_state) {
    case ORBIT_DISPLAY_LISTENING: speed = 60 + s_level * 300; sweep = 70 + s_level * 130; break;
    case ORBIT_DISPLAY_SPEAKING:  speed = 60 + s_level * 300; sweep = 90; break;
    case ORBIT_DISPLAY_OFFLINE:   speed = 30; sweep = 70; break;
    default:                      speed = 60; sweep = 70; break;
    }
    float a0 = t * speed;
    draw_arc(ANIM_CX, ANIM_CY, 10, a0, sweep);
    if (s_state == ORBIT_DISPLAY_SPEAKING) {
        draw_arc(ANIM_CX, ANIM_CY, 10, a0 + 180.0f, sweep); /* opposing arc */
    }
    if (s_level > 0.15f) {
        draw_arc(ANIM_CX, ANIM_CY, 6, -a0 * 1.5f, 30.0f); /* counter arc */
    }
}

static void draw_playing(float t)
{
    fill_circle(ANIM_CX, ANIM_CY, ANIM_R);
    /* music note knocked out of the centre */
    blit_glyph(ANIM_CX - ORBIT_NOTE_W / 2, ANIM_CY - ORBIT_NOTE_H / 2,
               ORBIT_NOTE, ORBIT_NOTE_W, ORBIT_NOTE_H, true);
    /* rotation cues: rim notch + radial tick (cleared pixels), 1 rev/s */
    float rad = t * 360.0f * (float)M_PI / 180.0f;
    for (float da = -4.0f; da <= 4.0f; da += 1.0f) {
        float r2 = rad + da * (float)M_PI / 180.0f;
        set_pixel(ANIM_CX + (int)lroundf(ANIM_R * cosf(r2)),
                  ANIM_CY + (int)lroundf(ANIM_R * sinf(r2)), 0);
        set_pixel(ANIM_CX + (int)lroundf((ANIM_R - 1) * cosf(r2)),
                  ANIM_CY + (int)lroundf((ANIM_R - 1) * sinf(r2)), 0);
    }
    for (int rr = 10; rr <= 13; rr++) {
        set_pixel(ANIM_CX + (int)lroundf(rr * cosf(rad)),
                  ANIM_CY + (int)lroundf(rr * sinf(rad)), 0);
    }
}

static void draw_thinking(float t)
{
    float phase = t * 90.0f; /* dashed ring rotates */
    for (int k = 0; k < 12; k++) {
        draw_arc(ANIM_CX, ANIM_CY, ANIM_R, phase + k * 30.0f, 20.0f);
    }
}

static void draw_alarm(float t)
{
    if ((s_frame / 12) % 2 == 0) { /* 1 Hz blink */
        draw_circle(ANIM_CX, ANIM_CY, ANIM_R);
        draw_arc(ANIM_CX, ANIM_CY, 10, t * 300.0f, 120.0f);
    }
}

static void draw_anim_zone(void)
{
    clear_zone(ANIM_COL0, ANIM_COL1);
    float t = (float)s_frame / 25.0f;
    switch (s_state) {
    case ORBIT_DISPLAY_PLAYING:  draw_playing(t); break;
    case ORBIT_DISPLAY_THINKING: draw_thinking(t); break;
    case ORBIT_DISPLAY_ALARM:    draw_alarm(t); break;
    default:                     draw_orb(t); break;
    }
}

/* ---------------- push ---------------- */

static void push_zone(uint8_t col0, uint8_t col1)
{
    int cols = col1 - col0 + 1;
    uint8_t *buf = heap_caps_malloc((size_t)cols * SSD1306_PAGES, MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    size_t o = 0;
    for (int p = 0; p < SSD1306_PAGES; p++) {
        for (int c = col0; c <= col1; c++) {
            buf[o++] = s_fb[p][c];
        }
    }
    ssd1306_write_window(col0, col1, 0, SSD1306_PAGES - 1, buf, o);
    free(buf);
}

/* ---------------- render task (25 FPS) ---------------- */

static void display_task(void *arg)
{
    while (1) {
        /* voice sensitivity: mic level, or output level while speaking/playing */
        float raw;
        if (s_manual_level) {
            raw = s_manual_level_val;
        } else if (s_state == ORBIT_DISPLAY_SPEAKING || s_state == ORBIT_DISPLAY_PLAYING) {
            raw = audio_pipeline_get_output_level();
        } else {
            raw = audio_pipeline_get_mic_level();
        }
        s_level = s_level * 0.7f + raw * 0.3f;

        /* dim in standby/offline */
        bool dim = (s_state == ORBIT_DISPLAY_STANDBY || s_state == ORBIT_DISPLAY_OFFLINE);
        uint8_t contrast = dim ? 0x40 : 0xCF;
        if (contrast != s_last_contrast) {
            ssd1306_set_contrast(contrast);
            s_last_contrast = contrast;
        }

        /* text zone: only when state or minute changed */
        if (s_text_dirty || s_minute != s_last_minute) {
            s_last_minute = s_minute;
            draw_text_zone();
            push_zone(TEXT_COL0, TEXT_COL1);
            s_text_dirty = false;
        }
        /* animation zone: every frame */
        draw_anim_zone();
        push_zone(ANIM_COL0, ANIM_COL1);

        s_frame++;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* ---------------- public API ---------------- */

esp_err_t orbit_display_init(void)
{
    esp_err_t err = ssd1306_init(ORBIT_DISPLAY_SDA_PIN, ORBIT_DISPLAY_SCL_PIN, 400000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED init failed — continuing without display");
        return err;
    }
    memset(s_fb, 0, sizeof(s_fb));
    push_zone(TEXT_COL0, TEXT_COL1);
    push_zone(ANIM_COL0, ANIM_COL1);
    if (xTaskCreate(display_task, "display", 8192, NULL, 4, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "display ready: 128x32 UI (logo · time · status · orb)");
    return ESP_OK;
}

/* ---------------- hardware test (hold BOOT at power-on) ---------------- */

void orbit_display_test_mode(bool on)
{
    s_test_mode = on;
}

void orbit_display_test_pattern(uint8_t phase)
{
    memset(s_fb, 0, sizeof(s_fb));
    switch (phase & 3) {
    case 0: /* clear */
        break;
    case 1: /* border + HW TEST */
        for (int x = 0; x < SSD1306_WIDTH; x++) {
            set_pixel(x, 0, 1);
            set_pixel(x, SSD1306_HEIGHT - 1, 1);
        }
        for (int y = 0; y < SSD1306_HEIGHT; y++) {
            set_pixel(0, y, 1);
            set_pixel(SSD1306_WIDTH - 1, y, 1);
        }
        draw_text(20, 13, "HW TEST");
        break;
    case 2: /* checkerboard */
        for (int y = 0; y < SSD1306_HEIGHT; y++) {
            for (int x = 0; x < SSD1306_WIDTH; x++) {
                if (((x ^ y) & 1) == 0) {
                    set_pixel(x, y, 1);
                }
            }
        }
        break;
    case 3: /* full on */
        memset(s_fb, 0xFF, sizeof(s_fb));
        break;
    }
    if ((phase & 3) == 0 && phase != 0) {
        phase = phase; /* unreachable */
    }
    if (phase == 4) {
        /* mic level bar (uses the manual level set via set_audio_level) */
        memset(s_fb, 0, sizeof(s_fb));
        draw_text(3, 13, "MIC");
        int w = (int)(s_manual_level_val * 80.0f);
        if (w < 0) w = 0;
        if (w > 80) w = 80;
        for (int x = 0; x < 80; x++) { /* frame */
            set_pixel(3 + x, 26, 1);
            set_pixel(3 + x, 30, 1);
        }
        set_pixel(3, 27, 1); set_pixel(3, 28, 1); set_pixel(3, 29, 1);
        set_pixel(82, 27, 1); set_pixel(82, 28, 1); set_pixel(82, 29, 1);
        for (int x = 0; x < w; x++) { /* bar */
            set_pixel(4 + x, 27, 1);
            set_pixel(4 + x, 28, 1);
            set_pixel(4 + x, 29, 1);
        }
    }
    push_zone(TEXT_COL0, TEXT_COL1);
    push_zone(ANIM_COL0, ANIM_COL1);
}

void orbit_display_set_state(orbit_display_state_t state)
{
    if (state != s_state) {
        s_state = state;
        s_text_dirty = true;
    }
}

void orbit_display_set_time(int hour, int minute)
{
    s_hour = hour;
    s_minute = minute;
    s_text_dirty = true;
}

void orbit_display_set_audio_level(float level01)
{
    /* negative disables the manual override */
    s_manual_level = level01 >= 0.0f;
    s_manual_level_val = level01 < 0.0f ? 0.0f : level01;
}
