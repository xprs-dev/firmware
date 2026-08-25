if(CONFIG_XPRS_BOARD_KV4P)
    idf_component_register(
        SRCS "model_init.c"
        INCLUDE_DIRS "."
        REQUIRES driver log nvs_flash xprs_sa818 xprs_aprs
        PRIV_REQUIRES freertos
    )
else()
    idf_component_register()
endif()
