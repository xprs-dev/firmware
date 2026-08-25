# The Reticulum bearer for the XPRS station -- see xprsrns.h. Registers empty
# when CONFIG_XPRS_BEARER_RNS is off: the header turns every call into a
# no-op, so xprs_app links the same on every board and small boards pay
# nothing.
if(CONFIG_XPRS_BEARER_RNS)
    set(srcs "xprsrns.c")
else()
    set(srcs "")
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES xprs_rns nvs_flash log esp_timer xprs_config xprs_codec)
