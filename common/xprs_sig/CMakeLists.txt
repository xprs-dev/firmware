# XPRS signatures (docs/XPRS.md §9.1) and opening sealed bodies (§6.2).
# secp256k1, SHA-256 and AES come from mbedtls on the device; test_*_host.*
# are HOST gcc harnesses (OpenSSL) run by their .sh and never compiled into
# the firmware. xprsseal.c is not in library.json: the P1-Pro has no use for
# it and its cut-down mbedtls carries no AES.
idf_component_register(
    SRCS "xprssig.c" "xprsseal.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls esp_hw_support log)
