# Diagnostics over the air (see xprs_diag.h). One implementation for every
# board; the app and the dongle register it with callbacks.
idf_component_register(
    SRCS "xprs_diag.c"
    INCLUDE_DIRS "."
    REQUIRES log esp_timer esp_hw_support esp_system app_update esp_app_format
             espcoredump xprs_codec xprs_auth xprs_health)
