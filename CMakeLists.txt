# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

file(STRINGS "version.txt" PROJECT_VER)

# Shape family drives the CMake project name, which becomes
# esp_app_desc_t.project_name in the image header and the basename of the
# .bin/.elf/.map. The OTA family check (main/ota_github.c) reads that field to
# refuse the other family's image. project() runs before sdkconfig is parsed,
# so the family is taken from the SDKCONFIG_DEFAULTS argument the caller
# passed, not from CONFIG_NINA_FAMILY_ROUND. A plain `idf.py build` with no
# argument is the square family, which is what every existing invocation means.
set(NINA_FAMILY "square")
if(DEFINED SDKCONFIG_DEFAULTS AND "${SDKCONFIG_DEFAULTS}" MATCHES "sdkconfig\\.defaults\\.round")
    set(NINA_FAMILY "round")
endif()
message(STATUS "NINA shape family: ${NINA_FAMILY}")

if(NINA_FAMILY STREQUAL "round")
    project(nina-display-round)
else()
    project(nina-display)
endif()

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
