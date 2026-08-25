# Serial console component for XPRS
# Provides CLI over UART for device control and debugging

# Base source files
set(CONSOLE_SRCS
    "console.c"
    "cmd_system.c"
    "cmd_wifi.c"
    "cmd_display.c"
    "cmd_config.c"
    "cmd_ssh.c"
    "cmd_ftp.c"
    "cmd_radio.c"
)

# Base requirements
set(CONSOLE_REQUIRES
    console esp_system driver nvs_flash log vfs
    xprs_node xprs_wifi xprs_json xprs_sdcard xprs_ssh xprs_ftp
    xprs_ble
)

set(CONSOLE_PRIV_REQUIRES
    xprs_model_epaper_1in54 xprs_shtc3 xprs_lvgl
    xprs_model_kv4p xprs_sa818
)

# Add mesh commands on targets that support ESP-MESH
if("${IDF_TARGET}" STREQUAL "esp32" OR "${IDF_TARGET}" STREQUAL "esp32s2" OR "${IDF_TARGET}" STREQUAL "esp32s3" OR "${IDF_TARGET}" STREQUAL "esp32c3")
    list(APPEND CONSOLE_SRCS "cmd_mesh.c")
    list(APPEND CONSOLE_PRIV_REQUIRES xprs_mesh xprs_nostr)
endif()

idf_component_register(
    SRCS ${CONSOLE_SRCS}
    INCLUDE_DIRS "." "../../include"
    REQUIRES ${CONSOLE_REQUIRES}
    PRIV_REQUIRES ${CONSOLE_PRIV_REQUIRES}
)
