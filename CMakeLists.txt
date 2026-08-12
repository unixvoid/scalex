# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Exclude legacy wifi_provisioning component
set(EXCLUDE_COMPONENTS wifi_provisioning)

project(midnight_scale)
