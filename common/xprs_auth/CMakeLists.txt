# May this packet make this station DO something (XPRS.md 25.4) -- one gate
# for the radio and for the HTTP door. No storage, no radio: it reads the
# allow-list out of config and does one signature check.
idf_component_register(
    SRCS "xprs_auth.c"
    INCLUDE_DIRS "."
    REQUIRES geogram_xprs geogram_xprsid geogram_xprssig geogram_nostr xprs_config log)
