# The modules shared with the hub are compiled from its main/ by path. A copy
# here would drift.
set(app_dir "${CMAKE_CURRENT_LIST_DIR}/../../main")

idf_component_register(
    SRCS
        "node_main.c"
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
        "${app_dir}/local_lights.c"
        "${app_dir}/pchip.c"
        "${app_dir}/thermal.c"
        "${app_dir}/ntc.c"
        "${app_dir}/radio_coex.c"
    INCLUDE_DIRS "." "${app_dir}"
    PRIV_REQUIRES
        app_update esp_adc esp_coex esp_driver_gpio esp_driver_ledc esp_driver_mcpwm esp_event
        esp_http_server esp_netif esp_system esp_timer esp_wifi ieee802154 esp_hal_ieee802154 lwip nvs_flash spi_flash)

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

# The first test images - see node_main.c.
if(NODE_TEST_MODE STREQUAL "zigbee")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NODE_TEST_ZIGBEE_ONLY)
elseif(NODE_TEST_MODE STREQUAL "hotspot")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NODE_TEST_HOTSPOT)
endif()
