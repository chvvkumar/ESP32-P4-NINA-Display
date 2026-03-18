# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

file(STRINGS "version.txt" PROJECT_VER)

project(lvgl_demo_v9)

# Build-time version header generation — runs git describe on every build.
# configure_file() inside the script only writes if content changed,
# so no unnecessary recompilation occurs when git state is unchanged.
add_custom_target(generate_build_version ALL
    COMMAND ${CMAKE_COMMAND}
        -D SRC_DIR=${CMAKE_SOURCE_DIR}
        -D DST_DIR=${CMAKE_BINARY_DIR}/generated
        -P ${CMAKE_SOURCE_DIR}/generate_version.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/build_version.h
    COMMENT "Generating build version header from git"
)
