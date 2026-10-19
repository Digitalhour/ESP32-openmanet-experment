idf_component_register(
    SRCS "probe_main.c" "batadv.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer lwip
)
