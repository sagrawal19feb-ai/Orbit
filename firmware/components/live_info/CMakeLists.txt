idf_component_register(SRCS "live_info.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client mbedtls nvs_flash esp_timer lwip json settings)
