# The Elixcon Light Node: a Zigbee end device driving wired lights and buttons.
#
# A project of its own, as the updater is, because CONFIG_ZB_ZED selects another
# Zigbee library at compile time. The modules it shares with the hub are compiled
# from ../main by path (see main/CMakeLists.txt here).
#
# It is built against the hub's partitions.csv, so the same updater image serves
# both and no data partition moves.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
set(ZBC_PARTITIONS_CSV "${CMAKE_CURRENT_LIST_DIR}/../partitions.csv")
# Only what main/ asks for: without this every ESP-IDF component is configured
# and linked. partition_layout is here for its project_include.cmake.
set(COMPONENTS main partition_layout)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(elixcon_light_node)
