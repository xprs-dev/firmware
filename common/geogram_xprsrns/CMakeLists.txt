# The Reticulum bearer for the XPRS station -- see xprsrns.h. Registers empty
# when CONFIG_GEOGRAM_XPRSRNS is off: the header turns every call into a
# no-op, so xprs_app links the same on every board and small boards pay
# nothing.
if(CONFIG_GEOGRAM_XPRSRNS)
    set(srcs "xprsrns.c")
else()
    set(srcs "")
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES geogram_rns nvs_flash log esp_timer xprs_config)
