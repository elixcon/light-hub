# The coordinator is built against partitions.csv with the updater row removed.
#
# partitions.csv puts a 1 MiB updater (app, factory) in front of the app
# (app, ota_0). Two things in ESP-IDF assume every app partition can hold the
# app, and against the real table both would break this project:
#
#  - check_sizes.py compares the image with *every* app partition and fails on
#    the smallest, which is the updater;
#  - `idf.py flash` writes the app to the boot-default partition, which is the
#    factory one - so the coordinator would be written over the updater.
#
# So the app is built against a derived table listing only what the app may be
# flashed to, and the root CMakeLists.txt puts the real table and the updater
# back into the flash targets. The derived table is generated at every
# configure and never committed: two tables kept by hand would drift, and the
# first sign would be a box whose flash layout is not the one it was built for.
#
# Both variables are set because the order project_include.cmake files are
# included in is not ours to choose. partition_table's own one reads
# CONFIG_PARTITION_TABLE_FILENAME to compute PARTITION_CSV_PATH: if this file
# runs first, the CONFIG override is what it reads; if second, the
# PARTITION_CSV_PATH override replaces what it computed.

set(ZBC_PARTITIONS_CSV "${PROJECT_DIR}/partitions.csv")
set(ZBC_PARTITIONS_APP_CSV "${BUILD_DIR}/partitions-app.csv")

file(STRINGS "${ZBC_PARTITIONS_CSV}" _zbc_rows)
set(_zbc_derived "# Generated from partitions.csv by components/partition_layout - do not edit.\n")
set(_zbc_dropped 0)
foreach(_zbc_row IN LISTS _zbc_rows)
    if(_zbc_row MATCHES "^[ \t]*[^#, \t][^,]*,[ \t]*app[ \t]*,[ \t]*factory[ \t]*,")
        math(EXPR _zbc_dropped "${_zbc_dropped} + 1")
    else()
        string(APPEND _zbc_derived "${_zbc_row}\n")
    endif()
endforeach()
if(NOT _zbc_dropped EQUAL 1)
    message(FATAL_ERROR "partitions.csv must have exactly one app,factory row - the updater - "
                        "and has ${_zbc_dropped}. See components/partition_layout/project_include.cmake.")
endif()

# Written only when it changes: a new timestamp would regenerate the partition
# table binary, and with it everything that depends on it, at every configure.
set(_zbc_old "")
if(EXISTS "${ZBC_PARTITIONS_APP_CSV}")
    file(READ "${ZBC_PARTITIONS_APP_CSV}" _zbc_old)
endif()
if(NOT _zbc_old STREQUAL _zbc_derived)
    file(WRITE "${ZBC_PARTITIONS_APP_CSV}" "${_zbc_derived}")
endif()

set(CONFIG_PARTITION_TABLE_FILENAME "${ZBC_PARTITIONS_APP_CSV}")
set(PARTITION_CSV_PATH "${ZBC_PARTITIONS_APP_CSV}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ZBC_PARTITIONS_CSV}")
