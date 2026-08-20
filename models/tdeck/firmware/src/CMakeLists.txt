idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver log geogram_st7789 geogram_i2c xprs_app)
