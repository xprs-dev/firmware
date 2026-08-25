# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

set(APP_REQUIRES xprs_ble xprs_ble_aprs nvs_flash xprs_ble_hello)

# FTP only on ePaper board (has SD card)
if(CONFIG_XPRS_BOARD_EPAPER_1IN54)
    list(APPEND APP_REQUIRES xprs_ftp)
endif()

# APRS-IS iGate + SD-backed message store only on the T-Dongle-S3
if(CONFIG_XPRS_BOARD_TDONGLE_S3)
    list(APPEND APP_REQUIRES xprs_aprsis xprs_sdcard xprs_msgstore)
endif()

idf_component_register(
    SRCS ${app_sources}
    REQUIRES ${APP_REQUIRES}
)
