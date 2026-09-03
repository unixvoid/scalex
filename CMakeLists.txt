# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Exclude legacy wifi_provisioning component
set(EXCLUDE_COMPONENTS wifi_provisioning)

# Version is sourced from git tags: ESP-IDF resolves PROJECT_VER automatically
# via `git describe --tags` when project() is called without a VERSION argument,
# so the git tag (e.g. v0.9.1) is the single source of truth.
# Clear stale cached values from older build configurations so the
# git-derived value is always used.
unset(PROJECT_VERSION CACHE)
unset(PROJECT_VER CACHE)

project(scalex)
