# Board-local component: only the M5Stack Core project compiles this, so no
# board-symbol source gating is needed (that dance exists for the shared
# components in common/, which every board's build enumerates).
idf_component_register(
    SRCS "ili9342.c"
    INCLUDE_DIRS "."
    REQUIRES driver log
    PRIV_REQUIRES freertos
)
