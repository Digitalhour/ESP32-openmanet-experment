idf_component_register(
    SRCS "hop_main.c" "mesh.c" "batadv.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event esp_timer lwip esp_driver_spi esp_driver_gpio
)

# esp-halow workarounds and the mesh layer's morselib hooks (see mesh.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=spi_bus_add_device")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmhal_wlan_hard_reset")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmwlan_set_channel_list")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_morse_cmd_tx")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_mmdrv_add_if")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_umac_ap_get_beacon")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_mmdrv_host_process_rx_frame")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_mmdrv_tx_frame")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_umac_rc_init_rate_table_mgmt")
