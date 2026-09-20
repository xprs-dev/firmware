# MeshCore on the XPRS LoRa radio: the wire, XPRS inside it, the repeater
# and the bridge. See mc.h and mc_mesh.h. test_mc_host.c and
# test_mc_mesh_host.c are a HOST gcc harness (test_mc_host.sh), never
# compiled into the firmware.
idf_component_register(
    SRCS "mc_wire.c" "mc_xprs.c" "mc_mesh.c"
    INCLUDE_DIRS "."
    REQUIRES xprs_codec xprs_loracrypto)
