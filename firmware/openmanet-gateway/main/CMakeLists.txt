idf_component_register(
    SRCS
        "app_main.c"
        "provisioning.c"
        "uplink_halow.c"
        "uplink_wifi.c"
        "downlink_softap.c"
        "downlink_halow_ap.c"
        "ip_forward_nat.c"
        "cot_relay.c"
        "web_ui.c"
        "status_led.c"
        "factory_reset.c"
        "log_buffer.c"
        "task_stats.c"
        "chip_temp.c"
        "link_history.c"
        "auth.c"
        "dns_forward.c"
        "heap_guard.c"
        "tls_identity.c"
        "radio_control.c"
        "net_validate.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        console
        lwip
        esp_http_server
        esp_https_server
        esp_timer
        esp_app_format
        app_update
        esp_driver_gpio
        json
    PRIV_REQUIRES
        # app_main.c reports the stored core dump's crashing task and backtrace
        # at boot (esp_core_dump.h). PRIV_ rather than REQUIRES because no
        # header this component installs exposes the type - it is an
        # implementation detail of app_main.c.
        espcoredump
        # chip_temp.c's ESP32-S3 die reading (esp_driver_tsens/temperature_sensor.h).
        # PRIV_ because only chip_temp.c touches the driver header - app_main.c
        # and web_ui.c go through chip_temp.h's own wrapper.
        esp_driver_tsens
        # auth.c's login verification is one mbedtls_md_hmac(SHA256) call over
        # the stored credential and a nonce - MBEDTLS_MD_C and MBEDTLS_SHA256_C,
        # both unconditionally compiled into this IDF version (no sdkconfig gate
        # exists for either - confirmed against
        # esp-idf/components/mbedtls/port/include/mbedtls/esp_config.h).
        # PRIV_ because only auth.c includes mbedtls/md.h. Deliberately does NOT
        # use mbedtls's own PBKDF2 (MBEDTLS_PKCS5_C): the KDF runs client-side
        # in web_ui.html's bundled JS, so this device only ever verifies one
        # precomputed HMAC - see design/ROADMAP.md item 1 and auth.c's own
        # comment for why running PBKDF2 on-device was rejected.
        mbedtls
)

# web_ui.html is embedded rather than served from a filesystem, so unlike the C
# sources - whose comments the preprocessor discards before codegen, costing
# zero flash - every byte of this file occupies a byte of the 3 MB app
# partition.
#
# So the embedded copy is minified *and* gzipped at build time instead of the
# source being thinned; web_ui.c serves it with `Content-Encoding: gzip`.
# Measured on the current file: 52,818 raw -> 44,362 minified -> 10,668
# gzipped. That is ~33 KB of the app partition returned, and the same 4x
# reduction in bytes on the wire for every page load - which happens over a
# SoftAP or HaLow link, not a LAN.
#
# Both stages, not one: gzip alone gives 12,245 bytes, so stripping comments
# first is still worth 12.9% on top of it. The generated file lands in the
# build directory and is never committed; main/web_ui.html keeps its comments.
# minify_web_ui.py documents why it only removes line-initial comments (the
# file contains regex literals a naive stripper would mangle) and fails loudly
# rather than guessing, and why it pins the gzip header's mtime to 0.
#
# This replaces `EMBED_FILES "web_ui.html"`, which cannot take a generated
# file. target_add_binary_data() is what EMBED_FILES calls internally
# (esp-idf/tools/cmake/component.cmake L512, with type "BINARY" - no trailing
# null byte, which matters because web_ui.c takes the length as end - start).
# RENAME_TO pins the symbol to `_binary_web_ui_html_start`/`_end` regardless of
# the generated filename, so web_ui.c:26-27 needs no change
# (esp-idf/tools/cmake/scripts/data_file_embed_asm.cmake builds the symbol from
# VARIABLE_BASENAME).
idf_build_get_property(python PYTHON)

set(web_ui_src "${CMAKE_CURRENT_LIST_DIR}/web_ui.html")
set(web_ui_minifier "${CMAKE_CURRENT_LIST_DIR}/minify_web_ui.py")
set(web_ui_embedded "${CMAKE_CURRENT_BINARY_DIR}/web_ui.min.html.gz")

add_custom_command(
    OUTPUT "${web_ui_embedded}"
    COMMAND "${python}" "${web_ui_minifier}" --gzip "${web_ui_src}" "${web_ui_embedded}"
    MAIN_DEPENDENCY "${web_ui_src}"
    DEPENDS "${web_ui_minifier}"
    COMMENT "Minifying and gzipping web_ui.html for embedding"
    VERBATIM)

target_add_binary_data(${COMPONENT_LIB} "${web_ui_embedded}" "BINARY"
    RENAME_TO web_ui_html
    DEPENDS "${web_ui_embedded}")
