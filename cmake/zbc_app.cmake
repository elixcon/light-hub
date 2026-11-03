# What the hub's and the node's main components build alike. Included after
# idf_component_register, whose COMPONENT_LIB these use.

# The driver's callbacks into the stack go through radio_coex.c, which counts
# every frame sent (by the driver's verdict) and received - see radio_coex.h.
# ieee802154_set_txrx_pti is the driver's own, called between its own files, and
# is wrapped to raise the send phase's coexistence priority; radio_coex.c takes
# its declaration from the driver's private header.
function(zbc_wrap_radio_coex radio_coex_c)
    foreach(zbc_sym esp_ieee802154_receive_done esp_ieee802154_transmit_done
                    esp_ieee802154_transmit_failed ieee802154_set_txrx_pti)
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${zbc_sym}")
    endforeach()
    idf_component_get_property(zbc_i154_dir ieee802154 COMPONENT_DIR)
    set_source_files_properties("${radio_coex_c}" PROPERTIES
        INCLUDE_DIRECTORIES "${zbc_i154_dir}/private_include")
endfunction()

# The UI is embedded gzipped and served with Content-Encoding: gzip - about a
# quarter of its size. The source stays plain HTML; this is the only build step
# it has.
#
# zopfli writes an ordinary gzip stream about 3.4 % smaller than gzip -9, in
# about a second. Brotli would save 14 %, but browsers accept it only over HTTPS
# and this page is plain HTTP. More iterations than the default 15 gain under
# 50 bytes. zopfli puts no name or timestamp in the header, so the image is
# reproducible.
function(zbc_embed_page)
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
endfunction()

# This build's full partition table (cmake/zbc_updater.cmake generates it for the
# flash targets). The firmware only trusts an updater that carries this table
# or the one on flash - see ota_updater_problem in ota.c.
#
# GENERATED has to be said here: target_add_binary_data makes the file a source
# of this directory, and under cmake_minimum_required(3.16) (CMP0118 OLD) the
# root's custom command that makes it is not seen from here. Without it a build
# directory where the file does not exist yet - a fresh clone, CI, a fullclean -
# stops at CMake's generate step with "Cannot find source file".
function(zbc_embed_full_table)
    idf_build_get_property(build_dir BUILD_DIR)
    set_source_files_properties("${build_dir}/partition_table/partition-table-full.bin" PROPERTIES GENERATED TRUE)
    target_add_binary_data(${COMPONENT_LIB} "${build_dir}/partition_table/partition-table-full.bin" BINARY
                           DEPENDS partition_table_full)
    add_dependencies(${COMPONENT_LIB} partition_table_full)
endfunction()
