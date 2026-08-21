cmake_minimum_required(VERSION 3.16)
# tinynimble is symlinked into components/ -- EXTRA_COMPONENT_DIRS pointing
# outside the project dir does not work under PlatformIO, the same note the
# model projects carry.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tinynimble_probe)
