idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver log ili9342 xprs_app)
