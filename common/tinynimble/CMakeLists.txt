# tinynimble: the BLE5 surface XPRS uses, spoken straight to the controller.
#
# tn_hci.c is deliberately free of ESP-IDF so the bytes that reach the radio are
# checked on a desk (test/test_tn_host.sh). tn_port_esp.c is the only file that
# knows about esp_bt_controller and esp_vhci.
#
# Requires `bt` for the controller. There is no NimBLE host here -- that is the
# 64,632 bytes of text and the mbuf pools this exists to not pay for.
#
# tn_port_esp.c includes esp_bt.h, and with CONFIG_BT_ENABLED=n the `bt`
# component registers with an EMPTY include_dirs -- so on a board with no
# Bluetooth it cannot compile. Drop it there and keep tn_hci.c, which is
# IDF-free by design and costs nothing: the component still resolves for
# anything that REQUIRES it, and geogram_xprsble stubs itself out under the
# same condition.
set(tn_srcs "tn_hci.c")
if(CONFIG_BT_ENABLED)
    list(APPEND tn_srcs "tn_port_esp.c")
endif()

idf_component_register(
    SRCS ${tn_srcs}
    INCLUDE_DIRS "."
    REQUIRES bt esp_common freertos log)
