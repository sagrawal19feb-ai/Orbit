idf_component_register(SRCS "alarm_timer.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash esp_timer json live_info)
