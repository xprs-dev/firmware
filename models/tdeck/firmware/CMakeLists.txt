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
idf_build_get_property(build_components BUILD_COMPONENT_TARGETS)
if("___idf_lvgl__lvgl" IN_LIST build_components)
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
