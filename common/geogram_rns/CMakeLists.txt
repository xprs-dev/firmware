# Reticulum codec — see rns.h. TweetNaCl (X25519 + Ed25519) lives HERE so every
# consumer shares one copy; rns_entropy.c is its randombytes() hook (ESP-IDF
# hardware RNG). test_rns_host.* are a HOST gcc harness (run test_rns_host.sh)
# and are never compiled into the firmware.
idf_component_register(
    SRCS "rns.c" "rns_tcp.c" "tweetnacl.c" "rns_entropy.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls esp_hw_support log lwip esp_timer)
