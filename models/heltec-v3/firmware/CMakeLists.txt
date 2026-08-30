cmake_minimum_required(VERSION 3.16)
# The shared components are symlinked into ./components, which PlatformIO/IDF
# auto-discover. EXTRA_COMPONENT_DIRS pointing OUTSIDE the project dir was
# tried and PlatformIO then failed to extract the components' include flags
# (st7735.h and the driver headers not found); in-project component dirs are
# handled natively. Same note as the T-Dongle, the T-Deck and the M5Stack.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(heltec_v3_xprs)
