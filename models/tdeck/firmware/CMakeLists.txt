cmake_minimum_required(VERSION 3.16)
# The shared components are symlinked into ./components, which PlatformIO/IDF
# auto-discover. EXTRA_COMPONENT_DIRS pointing outside the project dir does not
# work here — see the note in the M5Stack project's CMakeLists.txt.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tdeck_xprs)

# LVGL's TLSF pool is a plain array in .bss -- 50,249 B of internal DRAM on
# this board, which is the resource we are short of, not flash. LV_MEM_CUSTOM
# swaps it for our allocator, which serves LVGL out of PSRAM (see
# common/tinylv_mem/tinylv_mem.h). The four macros are #ifndef-guarded in
# lv_conf_internal.h, so defining them on the command line wins and the
# managed component stays unpatched.
# Asked of CMake directly. This used to be `"___idf_lvgl__lvgl" IN_LIST
# build_components`: three underscores where IDF uses two, and under
# PlatformIO that property is empty here anyway, so it was never true and
# LVGL fell back to plain malloc --
# internal DRAM only on this config (SPIRAM_USE_CAPS_ALLOC) -- so every
# object the UI ever made was taken from the heap this block exists to
# protect. Opening the Chat panel on a T-Deck cost 14.5 KB of it, for good,
# and a busy bench then ran the board to a 132-byte floor (2026-10-07).
# Check after a build: `grep -c tinylv_malloc .pio/build/<env>/*.map`.
if(TARGET __idf_lvgl__lvgl)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PRIVATE
        LV_MEM_CUSTOM_INCLUDE="tinylv_mem.h"
        LV_MEM_CUSTOM_ALLOC=tinylv_malloc
        LV_MEM_CUSTOM_FREE=tinylv_free
        LV_MEM_CUSTOM_REALLOC=tinylv_realloc)
    idf_component_get_property(tinylv_dir tinylv_mem COMPONENT_DIR)
    target_include_directories(${lvgl_lib} PRIVATE ${tinylv_dir})
    idf_component_get_property(tinylv_lib tinylv_mem COMPONENT_LIB)
    target_link_libraries(${lvgl_lib} PRIVATE ${tinylv_lib})
endif()
