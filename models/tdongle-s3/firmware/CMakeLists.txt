cmake_minimum_required(VERSION 3.16)
# The shared components are symlinked into ./components, which PlatformIO/IDF
# auto-discover. EXTRA_COMPONENT_DIRS pointing OUTSIDE the project dir was
# tried and PlatformIO then failed to extract the components' include flags
# (st7735.h and the driver headers not found); in-project component dirs are
# handled natively. The same note is in the T-Deck's and the M5Stack's.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tdongle_xprs)
