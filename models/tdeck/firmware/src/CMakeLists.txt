idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver log esp_adc xprs_st7789 xprs_i2c xprs_gt911 xprs_app xprs_script
             bt xprs_bearer_ble)
