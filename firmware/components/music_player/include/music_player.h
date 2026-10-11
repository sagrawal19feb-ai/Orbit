#pragma once

#include "esp_err.h"
#include <stdbool.h>

/*
 * Music player: Jamendo (full songs, free client ID) · Deezer (30 s previews)
 * · direct URL. HTTP stream → minimp3 decode → resample to 24 kHz → speaker.
 */

esp_err_t music_player_init(void);
esp_err_t music_player_play_query(const char *query); /* "play <song>" */
esp_err_t music_player_play_url(const char *url);     /* stream a direct link */
void music_player_stop(void);
void music_player_pause(void);
void music_player_resume(void);
bool music_player_is_playing(void);
