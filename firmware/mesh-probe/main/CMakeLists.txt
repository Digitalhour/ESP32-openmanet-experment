idf_component_register(
    SRCS "probe_main.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event esp_timer esp_driver_spi esp_driver_gpio
)

# Same esp-halow workarounds as HalowChat (SPI clock cap, longer chip reset, channel window).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=spi_bus_add_device")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmhal_wlan_hard_reset")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmwlan_set_channel_list")
# The probe itself: intercept morselib's chip command path so it can send its own
# ADD_INTERFACE requests and see every one morselib sends.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_morse_cmd_tx")
# Experiment 2: hand morselib's AP a mesh interface, rewrite beacons, and see raw scan frames.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_mmdrv_add_if")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_umac_ap_get_beacon")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_umac_scan_process_probe_resp")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_mmdrv_host_process_rx_frame")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mmint_mmdrv_tx_frame")
