# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Keep long compiler argument lists in response files on Windows.  This is
# required by Ninja when the project's component include paths exceed the
# CreateProcess command-line limit.
set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "Use response files for Ninja commands" FORCE)

set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "" FORCE)

set(PROJECT_VER "1.7.3")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi_chat)
