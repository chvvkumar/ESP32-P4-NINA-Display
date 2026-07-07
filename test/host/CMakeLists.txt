cmake_minimum_required(VERSION 3.16)
project(nina_host_tests C)

enable_testing()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Repo root (test/host is two levels below the repo root: <root>/test/host).
set(NINA_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(NINA_SHIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shims)

# ---------------------------------------------------------------------------
# Vendored cJSON (copied verbatim from the ESP-IDF component the firmware
# actually builds against: components/json/cJSON in esp-idf v5.5.2). Kept as
# its own static lib so test executables link it the same way firmware does.
# ---------------------------------------------------------------------------
add_library(cjson STATIC vendor/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC vendor/cJSON)

# ---------------------------------------------------------------------------
# Host shims: header-only stand-ins for ESP-IDF/FreeRTOS APIs, plus a small
# .c backing esp_timer_get_time()/shim_set_time_us(). Every host test that
# compiles unmodified firmware sources should link this.
# ---------------------------------------------------------------------------
add_library(host_shims STATIC shims/shims.c)
target_include_directories(host_shims PUBLIC ${NINA_SHIM_DIR} ${NINA_SHIM_DIR}/freertos)
if(WIN32)
    # QueryPerformanceCounter path in shims.c needs no extra libs beyond the
    # default Windows import libs CMake already links.
else()
    target_link_libraries(host_shims PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# add_nina_host_test(<name> <sources...>)
#
# Declares one test executable + matching ctest entry. Shim include dir is
# added FIRST so a shim header (e.g. shims/esp_log.h) always wins over any
# same-named header CMake might otherwise find later on the include path.
# Links cjson + host_shims by default; pass EXTRA args after a
# LINK_LIBRARIES keyword to add more (e.g. a per-test mock .c file).
# ---------------------------------------------------------------------------
function(add_nina_host_test NAME)
    set(multiValueArgs SOURCES LINK_LIBRARIES)
    cmake_parse_arguments(ARG "" "" "${multiValueArgs}" ${ARGN})
    if(NOT ARG_SOURCES)
        set(ARG_SOURCES ${ARGN})
    endif()

    add_executable(${NAME} ${ARG_SOURCES})
    target_include_directories(${NAME} PRIVATE
        ${NINA_SHIM_DIR}
        ${NINA_SHIM_DIR}/freertos
        ${NINA_REPO_ROOT}/main
        ${NINA_REPO_ROOT}/main/ui
    )
    target_link_libraries(${NAME} PRIVATE cjson host_shims ${ARG_LINK_LIBRARIES})
    if(NOT MSVC)
        target_link_libraries(${NAME} PRIVATE m)
    endif()
    target_compile_options(${NAME} PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
    )
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

# ---------------------------------------------------------------------------
# test_moon — wraps the existing test/moon/test_moon_compute.c against the
# real main/moon_ephemeris.c. moon_ephemeris.c has no ESP-IDF dependency
# (only <math.h>), so it needs none of the shims, but goes through the same
# add_nina_host_test() helper as every other host test for consistency.
# ---------------------------------------------------------------------------
add_nina_host_test(test_moon
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../moon/test_moon_compute.c
        ${NINA_REPO_ROOT}/main/moon_ephemeris.c
)

# ---------------------------------------------------------------------------
# test_session_stats — exercises the real main/ui/nina_session_stats.c
# (PSRAM ring buffers for RMS/HFR/temperature/star-count session analytics)
# against the host shims. Depends only on app_config.h (MAX_NINA_INSTANCES),
# display_defs.h, esp_log/esp_timer/esp_heap_caps/FreeRTOS shims -- all
# already provided by host_shims, no per-test mocks needed.
# ---------------------------------------------------------------------------
add_nina_host_test(test_session_stats
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_session_stats.c
        ${NINA_REPO_ROOT}/main/ui/nina_session_stats.c
)

# ---------------------------------------------------------------------------
# test_graph_downsample — wraps test/host/test_graph_downsample.c against the
# real main/ui/graph_downsample.c (stride/range math extracted from
# nina_graph_overlay.c). No ESP-IDF dependency (only <math.h>).
# ---------------------------------------------------------------------------
add_nina_host_test(test_graph_downsample
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_graph_downsample.c
        ${NINA_REPO_ROOT}/main/ui/graph_downsample.c
)

# ---------------------------------------------------------------------------
# test_nina_sequence — sequence/json tree walker (main/nina_sequence.c).
# http_get_json() and parse_iso8601() (declared in nina_client_internal.h,
# implemented in nina_client.c) are mocked directly inside the test file, so
# nina_client.c itself is NOT linked in.
# ---------------------------------------------------------------------------
add_nina_host_test(test_nina_sequence
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nina_sequence.c
        ${NINA_REPO_ROOT}/main/nina_sequence.c
)

# ---------------------------------------------------------------------------
# test_route_auth -- pure decision function for the default-deny web route
# auth gate (main/web_route_auth.c). No ESP-IDF dependency (only stdbool.h).
# ---------------------------------------------------------------------------
add_nina_host_test(test_route_auth
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_route_auth.c
        ${NINA_REPO_ROOT}/main/web_route_auth.c
)

# ---------------------------------------------------------------------------
# test_config_forward -- pure decision function for forward-tolerant config
# load (main/app_config_forward.h). Header-only, no ESP-IDF dependency
# (only stdint/stdbool/stddef).
# ---------------------------------------------------------------------------
add_nina_host_test(test_config_forward
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_config_forward.c
)

# ---------------------------------------------------------------------------
# test_http_policy -- pure decision functions for main/http_fetch.c
# (main/http_fetch_policy.h): redirect classification, retry-loop
# continuation, response-buffer sizing/growth. Header-only, no ESP-IDF
# dependency.
# ---------------------------------------------------------------------------
add_nina_host_test(test_http_policy
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_http_policy.c
)

# ---------------------------------------------------------------------------
# test_poll_backoff -- pure exponential-backoff step function for
# main/poll_task.c (main/poll_backoff.h). Header-only, no ESP-IDF dependency.
# ---------------------------------------------------------------------------
add_nina_host_test(test_poll_backoff
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_poll_backoff.c
)

# ---------------------------------------------------------------------------
# test_settings_table -- X-macro-driven defaults/clamp for the "simple"
# app_config_t fields (main/settings_table.c). themes_get_count() (the one
# runtime count_expr the table uses) is stubbed locally in the test file
# rather than linking the real main/ui/themes.c, which pulls in LVGL.
# ---------------------------------------------------------------------------
add_nina_host_test(test_settings_table
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_settings_table.c
        ${NINA_REPO_ROOT}/main/settings_table.c
)

# ---------------------------------------------------------------------------
# test_time_parse -- RFC-1123 HTTP Date header parser (main/time_parse.c).
# Pure standard C, no ESP-IDF dependency, no mocks needed.
# ---------------------------------------------------------------------------
add_nina_host_test(test_time_parse
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_time_parse.c
        ${NINA_REPO_ROOT}/main/time_parse.c
)
