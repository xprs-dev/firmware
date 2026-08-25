cmake_minimum_required(VERSION 3.16)
# Reuse the T-Dongle display components + LVGL from the main firmware tree. List
# the SPECIFIC dirs (not the whole ../components tree, which would register heavy
# unconditional components like xprs_wifi/mesh/http). xprs_common provides
# the Kconfig that defines CONFIG_XPRS_BOARD_TDONGLE_S3 (the guard the LCD/UI
# components compile under); xprs_lvgl provides lv_conf.h.
# The T-Dongle display components (xprs_st7735, xprs_model_tdongle_s3,
# xprs_tdongle_ui, xprs_common, xprs_lvgl) and the pinned LVGL are
# symlinked into ./components and ./managed_components, which PlatformIO/IDF
# auto-discover. EXTRA_COMPONENT_DIRS pointing OUTSIDE the project dir made
# PlatformIO fail to extract the components' include flags (st7735.h / driver
# headers not found); in-project component dirs are handled natively.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rns_ble5)
