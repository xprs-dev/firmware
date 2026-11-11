# The LoRa bearer. A board with no LoRa radio sets CONFIG_XPRS_LORA=n and
# gets xprslora_stub.c instead: the same API, nothing linked behind it
# (Kconfig). The headers of the mesh components stay on the include path,
# because xprslora.h names their types.
if(CONFIG_XPRS_LORA)
    set(srcs "xprslora.c" "lr_rotate.c" "lr_class.c" "lr_worth.c" "lr_probe.c" "lr_detect.c")
else()
    set(srcs "xprslora_stub.c" "lr_worth.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES xprs_codec xprs_bearer xprs_sx1262 xprs_meshtastic xprs_meshcore xprs_common log esp_timer
    PRIV_REQUIRES freertos esp_hw_support heap nvs_flash
)
