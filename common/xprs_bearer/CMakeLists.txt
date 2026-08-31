# The half of an XPRS bearer that is not about the radio — see xprsbearer.h.
#
# No lwip, no BLE, no ESP-NOW: xprs_codec and log only, so the whole file
# compiles on a host, which is where the timing and the §13.2.1 cancel rule are
# actually tested (xprs_bearer_lan/test_xprslan_host.sh).
idf_component_register(
    SRCS "xprsbearer.c" "xb_airtime.c"
    INCLUDE_DIRS "."
    REQUIRES xprs_codec log)
