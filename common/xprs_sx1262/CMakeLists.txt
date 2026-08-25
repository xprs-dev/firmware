# Unconditional since the T-Deck: any board may carry this radio, and the
# linker's --gc-sections drops it from the boards that reference none of it.
# (The old CONFIG_XPRS_BOARD_HELTEC_V3 gate was the multiboard scheme,
# where every board's build enumerated every component.)
idf_component_register(
    SRCS "sx1262.c"
    INCLUDE_DIRS "."
    REQUIRES driver log
    PRIV_REQUIRES freertos
)
