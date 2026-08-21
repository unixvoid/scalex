# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VERSION "0.0.1" CACHE STRING "Application semantic version")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Exclude legacy wifi_provisioning component
set(EXCLUDE_COMPONENTS wifi_provisioning)

project(scalex VERSION ${PROJECT_VERSION})
set(PROJECT_VER "${PROJECT_VERSION}" CACHE STRING "Application version embedded in the firmware")
