# generate_version.cmake — runs in script mode (-P) at BUILD time
# Produces build_version.h with fresh git metadata on every build.
# configure_file() only writes if content changed, preventing unnecessary recompilation.

find_package(Git QUIET)

# Read version.txt
if(EXISTS "${SRC_DIR}/version.txt")
    file(STRINGS "${SRC_DIR}/version.txt" PROJECT_VER)
else()
    set(PROJECT_VER "unknown")
endif()

# Git metadata — runs fresh every build
set(GIT_TAG "unknown")
set(GIT_SHA "unknown")
set(GIT_BRANCH "unknown")

if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${SRC_DIR}
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${SRC_DIR}
        OUTPUT_VARIABLE GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${SRC_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

# Only writes the file if content actually changed
configure_file(
    ${SRC_DIR}/main/build_version.h.in
    ${DST_DIR}/build_version.h
    @ONLY
)
