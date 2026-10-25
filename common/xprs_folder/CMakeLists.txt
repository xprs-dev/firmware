# Reading a published folder (XPRS.md 7.7.2, 7.7.3): the XFL1 listing, the
# folder and member descriptions, and which member is a newer firmware image
# for this board. Pure text; test_xfolder_host.c is the host harness (run
# test_xfolder_host.sh) and is not listed in SRCS.
idf_component_register(
    SRCS "xprs_folder.c"
    INCLUDE_DIRS "."
    REQUIRES xprs_codec
)
