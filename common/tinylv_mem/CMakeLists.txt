# LVGL's heap in PSRAM instead of internal DRAM -- see tinylv_mem.h. Depends
# only on the heap allocator, so it links anywhere.
idf_component_register(
    SRCS "tinylv_mem.c"
    INCLUDE_DIRS "."
    REQUIRES esp_common)
