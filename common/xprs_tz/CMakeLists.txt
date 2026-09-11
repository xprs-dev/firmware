# The station's time zone, asked of the internet (xprs_tz.h). Plain HTTP on
# an lwip socket; no HTTP client library and nothing that pulls TLS in.
idf_component_register(
    SRCS "xprs_tz.c"
    INCLUDE_DIRS "."
    REQUIRES lwip log)
