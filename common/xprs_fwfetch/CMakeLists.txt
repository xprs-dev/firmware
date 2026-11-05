# Firmware from the publisher's folder over Reticulum, unattended (xfw.h,
# docs/plan-firmware-folders.md). test_xfw_host.c is the host harness.
idf_component_register(
    SRCS "xfw.c"
    INCLUDE_DIRS "."
    REQUIRES xprs_codec xprs_id xprs_folder xprs_rns
)
