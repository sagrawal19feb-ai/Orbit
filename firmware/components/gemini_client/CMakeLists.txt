idf_component_register(SRCS "gemini_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES espressif__esp_websocket_client mcp_common mcp_router
                                esp_timer mbedtls json)
