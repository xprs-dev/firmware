# The engine behind the web chat's station API (XPRS.md 10.10): sessions,
# the administrator, moderation, hosted rooms, the alert policy. Plain C;
# test_xwc_host.sh runs it on the host. The httpd glue is xprs_api's.
idf_component_register(
    SRCS "xwc.c" "xwc_file.c"
    INCLUDE_DIRS "."
    REQUIRES xprs_codec xprs_id xprs_sig
)
