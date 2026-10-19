# The board, and nothing else. The station is xprs_app, shared with every
# other board; the UI is xprs_ui_none, the one that draws nothing, because
# this board has no screen (and no room for LVGL).
set(srcs "main.c")
# The c3qemu environment adds the stubs for hardware QEMU does not model
# (qemu_stubs.c); a board image never carries them.
if(XPRS_QEMU)
    list(APPEND srcs "qemu_stubs.c")
endif()
# The c3xwifi environment is the XPRS-WiFi relay: its own app_main in
# xwifi_relay.c, the station's main.c compiled out (see that file).
if(XPRS_XWIFI_RELAY)
    list(APPEND srcs "xwifi_relay.c")
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES driver log fatfs vfs wear_levelling xprs_app xprs_ui_none
             xprs_bearer_wifi8)
if(XPRS_XWIFI_RELAY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE XPRS_XWIFI_RELAY)
endif()
if(XPRS_QEMU)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=adc_hal_self_calibration")
endif()
