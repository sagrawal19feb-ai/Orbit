idf_component_register(SRCS "music_player.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client audio_pipeline minimp3 json settings)
