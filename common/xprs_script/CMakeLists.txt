# The Wrench script host: the VM's task, its PSRAM pool, its natives, and the
# signed-bundle loader (xs_bundle.h). C++ because Wrench's API is;
# -fno-exceptions/-fno-rtti so it cannot pull libstdc++ in behind us.
#
# xs_bundle.c is deliberately free of ESP-IDF so the code that decides whether
# foreign bytecode runs is the SAME code the host tests exercise on a desk.
# xs_sha256_idf.c is the one target-only piece.
idf_component_register(
    SRCS "xprs_script.cpp" "xs_bundle.c" "xs_sha256_idf.c"
    INCLUDE_DIRS "."
    REQUIRES geogram_wrench geogram_xprssig xprs_config xprs_health
             esp_partition mbedtls log esp_timer esp_common freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -fno-exceptions -fno-rtti -fno-threadsafe-statics)
