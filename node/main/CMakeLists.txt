# The modules shared with the hub are compiled from its main/ by path. A copy
# here would drift.
set(app_dir "${CMAKE_CURRENT_LIST_DIR}/../../main")

idf_component_register(
    SRCS
        "node_main.c"
        "node_mode.c"
        "node_store.c"
        "node_fallback.c"
        "node_buttons.c"
        "node_zb.c"
        "node_zcl.c"
        "node_state_rev.c"
        "node_http.c"
        "${app_dir}/btn_gesture.c"
        "${app_dir}/wifi_manager.c"
        "${app_dir}/captive_dns.c"
        "${app_dir}/settings.c"
        "${app_dir}/tz.c"
        "${app_dir}/light_db.c"
        "${app_dir}/button_db.c"
        "${app_dir}/crash_log.c"
        "${app_dir}/auth.c"
        "${app_dir}/auth_http.c"
        "${app_dir}/ieee_str.c"
        "${app_dir}/http_util.c"
        "${app_dir}/http_wired.c"
        "${app_dir}/ota.c"
        "${app_dir}/local_lights.c"
        "${app_dir}/pchip.c"
        "${app_dir}/thermal.c"
        "${app_dir}/ntc.c"
        "${app_dir}/radio_coex.c"
    INCLUDE_DIRS "." "${app_dir}"
    PRIV_REQUIRES
        app_update bootloader_support esp_app_format esp_bootloader_format esp_adc esp_coex esp_driver_gpio esp_driver_ledc esp_driver_mcpwm esp_event
        esp_http_server esp_netif esp_system esp_timer esp_wifi ieee802154 esp_hal_ieee802154 lwip mbedtls nvs_flash spi_flash)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/zbc_app.cmake")

zbc_wrap_radio_coex("${app_dir}/radio_coex.c")

# A crash leaves a record for the next boot, as on the hub (crash_log.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

zbc_embed_page()
zbc_embed_full_table()
