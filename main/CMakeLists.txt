idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "captive_dns.c"
        "device_db.c"
        "settings.c"
        "state_rev.c"
        "heap_watch.c"
        "crash_log.c"
        "config_backup.c"
        "auth.c"
        "auth_http.c"
        "group_db.c"
        "al_db.c"
        "sun.c"
        "tz.c"
        "ntc.c"
        "thermal.c"
        "colorspace.c"
        "remote.c"
        "curve_db.c"
        "button_db.c"
        "light_db.c"
        "mlight_db.c"
        "local_lights.c"
        "lighting.c"
        "buttons.c"
        "zb_coordinator.c"
        "http_server.c"
        "ota.c"
        "zb_ota_file.c"
        "zb_ota.c"
        "matter_bridge.cpp"
        "matter_fabric.cpp"
        "matter_commission.cpp"
        "matter_ctl.cpp"
        "radio_ed.c"
        "radio_coex.c"
        "wled.c"
    INCLUDE_DIRS "."
)

# The Zigbee stack's energy scans go through radio_ed.c, which starts again a
# scan the 802.15.4 driver never finishes - see radio_ed.h. --wrap redirects the
# stack's calls and the driver's callback; the originals stay reachable as
# __real_*. receive, transmit and sleep are wrapped only to see a scan abandoned;
# ed_failed (the driver's own weak default, which does nothing) to see one refused.
foreach(zbc_sym esp_ieee802154_energy_detect esp_ieee802154_energy_detect_done
                esp_ieee802154_receive esp_ieee802154_transmit esp_ieee802154_sleep
                esp_ieee802154_ed_failed)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${zbc_sym}")
endforeach()

# The driver's callbacks into the stack go through radio_coex.c, which counts
# every frame sent (by the driver's verdict) and received - see radio_coex.h.
# ieee802154_set_txrx_pti is the driver's own, called between its own files, and
# is wrapped to raise the send phase's coexistence priority; radio_coex.c takes
# its declaration from the driver's private header.
foreach(zbc_sym esp_ieee802154_receive_done esp_ieee802154_transmit_done
                esp_ieee802154_transmit_failed ieee802154_set_txrx_pti)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${zbc_sym}")
endforeach()
idf_component_get_property(zbc_i154_dir ieee802154 COMPONENT_DIR)
set_source_files_properties(radio_coex.c PROPERTIES
    INCLUDE_DIRECTORIES "${zbc_i154_dir}/private_include")

# A crash leaves a record for the next boot (crash_log.c): esp_panic_handler is
# wrapped so the registers, the task and the stack's code addresses are copied
# to RTC memory before IDF's handler prints them to a serial port nobody reads.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# With CONFIG_ZBC_HEAP_WATCH_TASKS the heap's per-task bookkeeping goes to
# heap_watch.c, which keeps a few counters per task where IDF keeps a list entry
# per block - see heap_watch.h. heap_watch.c takes heap_t and the three
# prototypes from the heap component's private headers.
if(CONFIG_ZBC_HEAP_WATCH_TASKS)
    foreach(zbc_sym heap_caps_update_per_task_info_alloc heap_caps_update_per_task_info_free
                    heap_caps_update_per_task_info_realloc)
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${zbc_sym}")
    endforeach()
    idf_component_get_property(zbc_heap_dir heap COMPONENT_DIR)
    set_source_files_properties(heap_watch.c PROPERTIES
        INCLUDE_DIRECTORIES "${zbc_heap_dir};${zbc_heap_dir}/private_include")
endif()

# The UI is embedded gzipped and served with Content-Encoding: gzip - about a
# quarter of its size. The source stays plain HTML; this is the only build step
# it has.
#
# zopfli writes an ordinary gzip stream about 3.4 % smaller than gzip -9, in
# about a second. Brotli would save 14 %, but browsers accept it only over HTTPS
# and this page is plain HTTP. More iterations than the default 15 gain under
# 50 bytes. zopfli puts no name or timestamp in the header, so the image is
# reproducible.
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

# This build's full partition table (the root CMakeLists.txt generates it for the
# flash targets). The coordinator only trusts an updater that carries this table
# or the one on flash - see ota_updater_problem in ota.c.
#
# GENERATED has to be said here: target_add_binary_data makes the file a source
# of this directory, and under cmake_minimum_required(3.16) (CMP0118 OLD) the
# root's custom command that makes it is not seen from here. Without it a build
# directory where the file does not exist yet - a fresh clone, CI, a fullclean -
# stops at CMake's generate step with "Cannot find source file".
idf_build_get_property(build_dir BUILD_DIR)
set_source_files_properties("${build_dir}/partition_table/partition-table-full.bin" PROPERTIES GENERATED TRUE)
target_add_binary_data(${COMPONENT_LIB} "${build_dir}/partition_table/partition-table-full.bin" BINARY
                       DEPENDS partition_table_full)
add_dependencies(${COMPONENT_LIB} partition_table_full)
