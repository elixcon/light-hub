# The updater: the factory app in partitions.csv. See main/updater.c.
#
# Normally built by the coordinator's own build, as a subproject, and flashed by
# its `idf.py flash`. `idf.py -C updater build` works too.
cmake_minimum_required(VERSION 3.16)

# Only what main/ asks for. Without this every ESP-IDF component is configured
# and linked, and what does not reach the image still costs build time.
# ieee802154 and esp_coex are not optional - see the coexistence call in
# main/updater.c.
set(COMPONENTS main ieee802154 esp_coex)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(elixcon_light_hub_updater)
