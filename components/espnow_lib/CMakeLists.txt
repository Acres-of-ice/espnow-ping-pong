idf_component_register(
    SRCS "espnow_lib.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_timer
)
