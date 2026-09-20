# The curve and cipher primitives the LoRa networks share (xlc.h). The host
# harness (test_xlc_host.sh) compiles the same sources with gcc and fills the
# two platform seams from OpenSSL, so nothing here is IDF-only except the
# two *_idf.c files.
idf_component_register(
    SRCS "xlc_x25519.c" "xlc_ed25519.c" "xlc_hmac.c" "xlc_aes_idf.c"
         "xlc_sha512_idf.c"
    INCLUDE_DIRS "."
    REQUIRES xprs_codec mbedtls)
