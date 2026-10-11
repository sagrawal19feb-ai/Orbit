idf_component_register(SRCS "wifi_config.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_http_server esp_netif esp_event nvs_flash
                                app_update settings live_info esp_timer)
