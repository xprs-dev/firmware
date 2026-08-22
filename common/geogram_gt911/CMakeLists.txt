# GT911 capacitive touch, polled. See gt911.h for why there is no interrupt.
idf_component_register(
    SRCS "gt911.c"
    INCLUDE_DIRS "."
    REQUIRES geogram_i2c log)
