idf_component_register(SRCS "mcp_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mcp_common esp_http_client mbedtls nvs_flash esp_timer json)
