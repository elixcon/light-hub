idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "captive_dns.c"
        "ieee_str.c"
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
        "pchip.c"
        "curve_db.c"
        "button_db.c"
        "light_db.c"
        "mlight_db.c"
        "mexp_db.c"
        "local_lights.c"
        "lighting.c"
        "btn_gesture.c"
        "buttons.c"
        "zb_coordinator.c"
        "http_util.c"
        "http_wired.c"
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
        "icmp_limit.c"
        "wled.c"
    INCLUDE_DIRS "."
)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/zbc_app.cmake")

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

zbc_wrap_radio_coex(radio_coex.c)

# lwIP's ICMP errors - "port unreachable" for every datagram nobody listens for -
# go out at a bounded rate (icmp_limit.c), so a flood cannot fill the Wi-Fi
# transmit queue with replies. Nothing else calls into icmp_limit.c, and lwIP's
# references come after libmain.a in the link, so the wrappers are named as
# undefined up front to pull the file in.
foreach(zbc_sym icmp_dest_unreach icmp6_dest_unreach)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${zbc_sym}"
                                                     "-Wl,--undefined=__wrap_${zbc_sym}")
endforeach()

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

zbc_embed_page()
zbc_embed_full_table()
