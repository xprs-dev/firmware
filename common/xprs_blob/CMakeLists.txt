# XBLOB -- a lean 1:1 GATT bulk-transfer protocol (docs/ble5-gatt.md).
#
# Pure protocol logic: manifest + per-block hashes, a windowed parcel blast,
# and receiver-driven NEED-bitmap recovery of missing/corrupt blocks. NO radio,
# storage or OS code -- the firmware binds a transport (one send()) and, for
# OTA, the flash spool. Its one dependency is the sha256 seam in xprs_codec.
#
# test_xblob_host.c is a HOST-ONLY gcc harness (see its header) and must stay
# out of SRCS.
idf_component_register(
    SRCS "xprs_blob.c"
    INCLUDE_DIRS "."
    REQUIRES xprs_codec
)
