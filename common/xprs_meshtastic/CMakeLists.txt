# Meshtastic on the XPRS LoRa radio: the wire, the repeater, the bridge.
# See mt.h. test_mt_host.c and test_mt_mesh_host.c are a HOST gcc harness
# (test_mt_host.sh), never compiled into the firmware.
idf_component_register(
    SRCS "mt_wire.c" "mt_xprs.c" "mt_mesh.c" "mt_pki.c"
    INCLUDE_DIRS "."
    REQUIRES xprs_codec xprs_loracrypto mbedtls)
