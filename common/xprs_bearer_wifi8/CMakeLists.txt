# XPRS over XPRS-WiFi (the long-range CSS bearer) — see xprswifi8.h.
#
# Like xprsnow, the queue, the identifier rings and the §13.2.1 cancel are
# xprs_bearer, shared with every other bearer; this component is the radio, the
# time-slice scheduler that shares it with 802.11, and nothing else. It pulls in
# the xwifi library (core + the chip's port) through the `xwifi` component.
#
# Gated by CONFIG_XPRS_WIFI8, which Kconfig only offers on esp32c3 and esp32s3,
# the chips xwifi has a port for. On every other target this component registers
# empty, so a board that links it on an unsupported chip still builds.
#
# REQUIRES stays the SAME in both branches on purpose: ESP-IDF expands a
# component's requirements with an early pass that runs this file before
# sdkconfig is loaded, so CONFIG_XPRS_WIFI8 reads false there regardless of
# the real setting -- a REQUIRES that only appeared in the true branch was
# silently dropped, and xprswifi8.c then failed to find xprsbearer.h even
# though the true branch's SRCS did compile. Only SRCS may depend on the
# config; it is read again, correctly, in the real pass.
if(CONFIG_XPRS_WIFI8)
    set(wifi8_srcs "xprswifi8.c")
else()
    set(wifi8_srcs)
endif()
idf_component_register(
    SRCS ${wifi8_srcs}
    INCLUDE_DIRS "."
    REQUIRES xwifi xprs_codec xprs_bearer xprs_common esp_wifi esp_timer esp_hw_support log)
