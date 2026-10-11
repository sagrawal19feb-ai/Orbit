idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES orbit_board assets_reader audio_pipeline orbit_display
                                wake_word gemini_client music_player alarm_timer settings
                                wifi_config nvs_flash spi_flash esp_hw_support esp_timer
                                mcp_common mcp_local mcp_client mcp_router live_info json)
