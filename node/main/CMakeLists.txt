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
        "node_debug.c"
        "${app_dir}/btn_gesture.c"
        "${app_dir}/wifi_manager.c"
        "${app_dir}/captive_dns.c"
        "${app_dir}/settings.c"
        "${app_dir}/tz.c"
        "${app_dir}/light_db.c"
        "${app_dir}/button_db.c"
        "${app_dir}/crash_log.c"
        "${app_dir}/auth.c"
        "${app_dir}/local_lights.c"
        "${app_dir}/pchip.c"
        "${app_dir}/thermal.c"
        "${app_dir}/ntc.c"
        "${app_dir}/radio_coex.c"
    INCLUDE_DIRS "." "${app_dir}"
    PRIV_REQUIRES
        app_update esp_adc esp_coex esp_driver_gpio esp_driver_ledc esp_driver_mcpwm esp_event
        esp_http_server esp_netif esp_system esp_timer esp_wifi ieee802154 esp_hal_ieee802154 lwip mbedtls nvs_flash spi_flash)

# The driver's callbacks go through radio_coex.c, as on the hub: every frame counted
# by the driver's verdict, and the send phase's coexistence priority raised for the
# times Wi-Fi is on. It takes ieee802154_set_txrx_pti from the driver's private header.
foreach(zbc_sym esp_ieee802154_receive_done esp_ieee802154_transmit_done
                esp_ieee802154_transmit_failed ieee802154_set_txrx_pti)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${zbc_sym}")
endforeach()
idf_component_get_property(zbc_i154_dir ieee802154 COMPONENT_DIR)
set_source_files_properties("${app_dir}/radio_coex.c" PROPERTIES
    INCLUDE_DIRECTORIES "${zbc_i154_dir}/private_include")

# A crash leaves a record for the next boot, as on the hub (crash_log.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# -DNODE_TEST_WIFI=1: boots with Wi-Fi up, so a test image can be watched.
if(NODE_TEST_WIFI)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NODE_TEST_WIFI)
endif()
