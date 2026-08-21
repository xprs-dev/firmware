# One definition of "this station is working" (see xprs_health.h), used by
# the boot log, the heartbeat and the OTA rollback self-test alike so they
# cannot drift apart. No dependencies beyond logging and the heap.
idf_component_register(
    SRCS "xprs_health.c"
    INCLUDE_DIRS "."
    REQUIRES log esp_common)
