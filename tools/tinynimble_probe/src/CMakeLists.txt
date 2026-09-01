# One probe, two stacks. PROBE_NIMBLE picks the reference implementation so the
# A/B runs the same test on the same board with only the stack changed.
if(DEFINED ENV{PROBE_NIMBLE})
    set(RADIO_SRC "radio_nimble.c")
    set(RADIO_REQ bt)
else()
    set(RADIO_SRC "radio_tiny.c")
    set(RADIO_REQ tinynimble)
endif()
idf_component_register(SRCS "main.c" ${RADIO_SRC}
                       INCLUDE_DIRS "."
                       REQUIRES ${RADIO_REQ} esp_timer xprs_blob)
