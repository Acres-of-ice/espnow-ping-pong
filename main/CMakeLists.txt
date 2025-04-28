idf_component_register(
    SRCS "espnow_example_main.c" "web.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash espnow_lib esp_http_server 
)
