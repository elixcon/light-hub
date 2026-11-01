# The following lines of boilerplate have to be at the top of every project CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(elixcon_light_hub)

# The updater, and the real partition table in the flash targets - shared with
# node/, which builds the same updater beside its own image.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/zbc_updater.cmake")
