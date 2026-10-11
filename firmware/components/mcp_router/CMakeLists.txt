idf_component_register(SRCS "mcp_router.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mcp_common mcp_local mcp_client json freertos)
