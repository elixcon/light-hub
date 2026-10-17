# The following lines of boilerplate have to be at the top of every project CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(elixcon_light_hub)

# ------------------------------------------------------------------ the updater
#
# partitions.csv has two app partitions: `updater` (factory, 1 MiB) and `app`
# (ota_0), and this project is the app. updater/ is a project of its own, built
# here as a subproject the way ESP-IDF builds the bootloader, so one `idf.py
# build` produces both images and one `idf.py flash` writes both.
#
# The app is built against a table without the updater row - see
# components/partition_layout for why - so the flash targets are given the real
# table and the updater image back here. Without that, `idf.py flash` would
# write a table the updater is missing from, and the first failed update would
# have nowhere to roll back to.

idf_build_get_property(build_dir BUILD_DIR)
idf_build_get_property(idf_path IDF_PATH)
idf_build_get_property(idf_target IDF_TARGET)
idf_build_get_property(python PYTHON)
idf_build_get_property(sdkconfig_cmake SDKCONFIG_CMAKE)
include(${sdkconfig_cmake})

set(zbc_table_csv "${CMAKE_CURRENT_LIST_DIR}/partitions.csv")
set(zbc_table_bin "${build_dir}/partition_table/partition-table-full.bin")
set(zbc_updater_dir "${build_dir}/updater")
set(zbc_updater_bin "${zbc_updater_dir}/elixcon_light_hub_updater.bin")

include(ExternalProject)
externalproject_add(updater
    SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/updater"
    BINARY_DIR "${zbc_updater_dir}"
    CMAKE_ARGS -DIDF_PATH=${idf_path} -DIDF_TARGET=${idf_target}
               -DPYTHON=${python} -DPYTHON_DEPS_CHECKED=1
               -DSDKCONFIG=${zbc_updater_dir}/sdkconfig
               -DCCACHE_ENABLE=${CCACHE_ENABLE}   # idf.py's --ccache reaches only this project
    INSTALL_COMMAND ""
    BUILD_ALWAYS 1   # its own build decides whether anything changed
    BUILD_BYPRODUCTS "${zbc_updater_bin}")
add_dependencies(app updater)

# The real table, built with the same options partition_table uses for the
# derived one.
if(CONFIG_PARTITION_TABLE_MD5)
    set(zbc_md5_opt "")
else()
    set(zbc_md5_opt "--disable-md5sum")
endif()
add_custom_command(OUTPUT "${zbc_table_bin}"
    COMMAND ${python} "${idf_path}/components/partition_table/gen_esp32part.py" -q
            --offset ${CONFIG_PARTITION_TABLE_OFFSET}
            --primary-bootloader-offset ${CONFIG_BOOTLOADER_OFFSET_IN_FLASH}
            ${zbc_md5_opt} --flash-size ${CONFIG_ESPTOOLPY_FLASHSIZE}
            -- "${zbc_table_csv}" "${zbc_table_bin}"
    DEPENDS "${zbc_table_csv}"
    VERBATIM)
add_custom_target(partition_table_full ALL DEPENDS "${zbc_table_bin}")

execute_process(
    COMMAND ${python} "${idf_path}/components/partition_table/parttool.py" -q
            --partition-table-offset ${CONFIG_PARTITION_TABLE_OFFSET}
            --primary-bootloader-offset ${CONFIG_BOOTLOADER_OFFSET_IN_FLASH}
            --partition-table-file "${zbc_table_csv}"
            get_partition_info --partition-type app --partition-subtype factory --info offset
    OUTPUT_VARIABLE zbc_updater_offset
    RESULT_VARIABLE zbc_rc
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT zbc_rc EQUAL 0 OR NOT zbc_updater_offset)
    message(FATAL_ERROR "partitions.csv has no app,factory partition for the updater")
endif()

# Swap the derived table for the real one wherever it would be flashed. Checked,
# because this leans on how esptool_py records its images: if a future ESP-IDF
# records them differently the build must stop rather than quietly flash the
# derived table.
foreach(zbc_target flash partition-table-flash)
    foreach(zbc_prop FLASH_FILE FLASH_ENTRY IMAGES)
        get_property(zbc_val TARGET ${zbc_target} PROPERTY ${zbc_prop})
        string(FIND "${zbc_val}" "partition_table/partition-table.bin" zbc_at)
        if(zbc_at EQUAL -1)
            message(FATAL_ERROR "Could not find the partition table in ${zbc_target}'s ${zbc_prop}; "
                                "see the root CMakeLists.txt")
        endif()
        string(REPLACE "partition_table/partition-table.bin" "partition_table/partition-table-full.bin"
               zbc_val "${zbc_val}")
        set_property(TARGET ${zbc_target} PROPERTY ${zbc_prop} "${zbc_val}")
    endforeach()
    add_dependencies(${zbc_target} partition_table_full)
endforeach()

idf_component_get_property(zbc_main_args esptool_py FLASH_ARGS)
idf_component_get_property(zbc_sub_args esptool_py FLASH_SUB_ARGS)
esptool_py_flash_target(updater-flash "${zbc_main_args}" "${zbc_sub_args}")
esptool_py_flash_target_image(updater-flash updater "${zbc_updater_offset}" "${zbc_updater_bin}")
esptool_py_flash_target_image(flash updater "${zbc_updater_offset}" "${zbc_updater_bin}")
add_dependencies(updater-flash updater)
add_dependencies(flash updater)
