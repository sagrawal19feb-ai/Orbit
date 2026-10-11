idf_component_register(SRCS "mcp_local.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mcp_common music_player alarm_timer audio_pipeline orbit_board live_info settings)
