# Read-only blobs from a raw flash partition (see xasset.h). No VFS, no
# FATFS, no heap: the splash has to be drawable before a filesystem exists
# and must not cost the 4-8 KB that mounting one does.
idf_component_register(
    SRCS "xasset.c"
    INCLUDE_DIRS "."
    REQUIRES log esp_partition)
