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

# The page, embedded gzipped - see main/CMakeLists.txt, which this follows.
find_program(ZOPFLI zopfli REQUIRED)
set(index_gz "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(
    OUTPUT "${index_gz}"
    COMMAND sh -c "\"$0\" -c \"$1\" > \"$2\"" "${ZOPFLI}" "${CMAKE_CURRENT_SOURCE_DIR}/www/index.html" "${index_gz}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/www/index.html"
    VERBATIM)
add_custom_target(index_html_gz DEPENDS "${index_gz}")
add_dependencies(${COMPONENT_LIB} index_html_gz)
target_add_binary_data(${COMPONENT_LIB} "${index_gz}" BINARY)

# This build's full partition table, so the updater it ships with is trusted before
# that table is on flash - see ota_updater_problem in ota.c, and main/CMakeLists.txt.
idf_build_get_property(build_dir BUILD_DIR)
set_source_files_properties("${build_dir}/partition_table/partition-table-full.bin" PROPERTIES GENERATED TRUE)
target_add_binary_data(${COMPONENT_LIB} "${build_dir}/partition_table/partition-table-full.bin" BINARY
                       DEPENDS partition_table_full)
add_dependencies(${COMPONENT_LIB} partition_table_full)
