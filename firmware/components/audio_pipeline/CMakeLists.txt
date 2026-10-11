idf_component_register(SRCS "audio_pipeline.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2s orbit_board esp_timer)
