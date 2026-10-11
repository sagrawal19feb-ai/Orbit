idf_component_register(SRCS "orbit_display.c" "ssd1306.c"
                       INCLUDE_DIRS "include" "."
                       REQUIRES esp_driver_i2c orbit_board audio_pipeline esp_timer freertos)
