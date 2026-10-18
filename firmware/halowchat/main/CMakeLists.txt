idf_component_register(
    SRCS "chat_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_http_client esp_timer lwip esp_driver_spi esp_driver_gpio json
    EMBED_TXTFILES "chat_ui.html"
)

# Same esp-halow workarounds as the Ottercam firmware (see its READMEs).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=spi_bus_add_device")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmhal_wlan_hard_reset")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmwlan_tx_pkt")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmwlan_set_channel_list")
