# The battery gauge and the battery policy, shared by every board that can
# measure its own cell. It is required by xprs_app rather than by any board,
# so a model gets it without naming it -- the same way xprs_config and
# xprs_health arrive.
#
# esp_pm is here for one call, esp_pm_configure(). It is not gated on
# CONFIG_PM_ENABLE: the header and the symbol exist either way, and with PM
# off the call answers ESP_ERR_NOT_SUPPORTED, which xprs_power.c logs and
# carries on from. A guarded REQUIRES would be worse than useless -- IDF
# expands component requirements before Kconfig is loaded, so it would be
# dropped from the dependency graph entirely (see xprs_model_heltec_v3).
idf_component_register(
    SRCS "xprs_power.c"
    INCLUDE_DIRS "."
    REQUIRES log nvs_flash esp_pm)
