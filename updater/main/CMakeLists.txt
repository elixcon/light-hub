# The modules shared with the coordinator are compiled from its main/ by path.
# A copy here would drift, and the first sign would be an updater that can no
# longer read the Wi-Fi credentials or the keys the coordinator wrote.
set(app_dir "${CMAKE_CURRENT_LIST_DIR}/../../main")

idf_component_register(
    SRCS
        "updater.c"
        "updater_http.c"
        "convert.c"
        "stubs.c"
        "${app_dir}/wifi_manager.c"
        "${app_dir}/captive_dns.c"
        "${app_dir}/settings.c"
        "${app_dir}/tz.c"
        "${app_dir}/auth.c"
        "${app_dir}/auth_http.c"
        "${app_dir}/ota.c"
    INCLUDE_DIRS "." "${app_dir}"
    PRIV_REQUIRES
        app_update bootloader_support esp_app_format esp_bootloader_format esp_event esp_http_server
        esp_coex esp_netif esp_partition esp_system esp_timer esp_wifi ieee802154 lwip mbedtls
        nvs_flash spi_flash
    EMBED_TXTFILES "page.html")

# What convert.c writes when it converts another firmware's flash: this build's
# own partition table (partitions.csv, the real one) and bootloader. Taken from
# the build rather than committed, so the layout a conversion produces is always
# the layout a USB flash produces.
#
# Both are made in other directories (IDF's partition_table and bootloader
# components), so they are marked GENERATED here, or a build directory without
# them yet stops at CMake's generate step - see main/CMakeLists.txt.
idf_build_get_property(build_dir BUILD_DIR)
set_source_files_properties("${build_dir}/partition_table/partition-table.bin"
                            "${build_dir}/bootloader/bootloader.bin" PROPERTIES GENERATED TRUE)
target_add_binary_data(${COMPONENT_LIB} "${build_dir}/partition_table/partition-table.bin" BINARY
                       DEPENDS partition_table_bin)
target_add_binary_data(${COMPONENT_LIB} "${build_dir}/bootloader/bootloader.bin" BINARY
                       DEPENDS bootloader)
add_dependencies(${COMPONENT_LIB} partition_table_bin bootloader)
