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
# anything that REQUIRES it, and xprs_bearer_ble stubs itself out under the
# same condition.
# tn_att.c is the ATT server for the mesh channel (docs/ble5-gatt.md). It is
# IDF-free like tn_hci.c and checked the same way (test/test_tn_att_host.sh);
# nothing calls it yet, so --gc-sections drops it from every image until the
# port layer grows ACL data. It is compiled everywhere so it cannot rot.
set(tn_srcs "tn_hci.c" "tn_att.c")
if(CONFIG_BT_ENABLED)
    list(APPEND tn_srcs "tn_port_esp.c")
endif()

idf_component_register(
    SRCS ${tn_srcs}
    INCLUDE_DIRS "."
    REQUIRES bt esp_common freertos log)
