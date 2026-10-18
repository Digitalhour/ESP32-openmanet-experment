idf_component_register(
    SRCS "chat_main.c" "mesh.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_http_client esp_timer lwip esp_driver_spi esp_driver_gpio json
    EMBED_TXTFILES "chat_ui.html"
)

# esp-halow workarounds (SPI clock cap, longer chip reset, channel window); see mesh.c.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=spi_bus_add_device")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmhal_wlan_hard_reset")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmwlan_set_channel_list")
# The mesh layer hooks into morselib (the esp-halow build prefixes its internals with mmint_):
# the chip command path, interface creation, beacon construction, the receive entry and the
# transmit entry. See mesh.c and the README for what each one does.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_morse_cmd_tx")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_mmdrv_add_if")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_umac_ap_get_beacon")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_mmdrv_host_process_rx_frame")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_mmdrv_tx_frame")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_umac_rc_init_rate_table_mgmt")
