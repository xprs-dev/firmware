idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver log esp_adc geogram_st7789 geogram_i2c geogram_gt911 xprs_app xprs_script
             bt geogram_xprsble)
