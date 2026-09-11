cmake_minimum_required(VERSION 3.16)
# The shared components are symlinked into ./components, which PlatformIO/IDF
# auto-discover (EXTRA_COMPONENT_DIRS outside the project dir does not work
# here; see the note in the T-Dongle's and the M5Stack's CMakeLists.txt).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_generic_xprs)
