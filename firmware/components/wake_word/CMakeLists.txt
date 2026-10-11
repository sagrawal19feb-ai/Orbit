idf_component_register(SRCS "wake_word.c"
                       INCLUDE_DIRS "include"
                       REQUIRES espressif__esp-sr audio_pipeline esp_timer)
