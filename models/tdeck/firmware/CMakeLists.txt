cmake_minimum_required(VERSION 3.16)
# The shared components are symlinked into ./components, which PlatformIO/IDF
# auto-discover. EXTRA_COMPONENT_DIRS pointing outside the project dir does not
# work here — see the note in the M5Stack project's CMakeLists.txt.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tdeck_xprs)
