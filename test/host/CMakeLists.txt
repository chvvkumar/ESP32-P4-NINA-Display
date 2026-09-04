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
# test_image_probe -- magic-byte sniffing + PNG/GIF header dimension reads
# (main/image_probe.h), the gate main/jpeg_utils.c applies to fetched payloads
# before decoding. Header-only, no ESP-IDF dependency: jpeg_utils.c itself
# cannot be host-compiled (driver/ppa.h, esp_cache.h, stb_image.h), which is
# why the pure byte-offset/endianness logic lives in its own header.
# ---------------------------------------------------------------------------
add_nina_host_test(test_image_probe
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_image_probe.c
)

# ---------------------------------------------------------------------------
# test_radar_sites -- nearest WSR-88D site lookup (main/radar_sites.c), which
# resolves an empty radar_token into a RIDGE image token. Pure float math over
# a baked table; depends only on math.h, so no shims are strictly needed but it
# goes through add_nina_host_test() like everything else.
# ---------------------------------------------------------------------------
add_nina_host_test(test_radar_sites
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_radar_sites.c
        ${NINA_REPO_ROOT}/main/radar_sites.c
)

# ---------------------------------------------------------------------------
# test_radar_play -- pure ring/playback decisions for the radar animation
# (main/radar_play.h): FNV-1a frame dedupe, the oldest->newest playback cursor
# and the newest-frame hold. Header-only, no ESP-IDF dependency; the buffer
# ownership and LVGL handoff it drives live in ui/nina_image_page.c and cannot
# be host-compiled, which is why the decisions sit in their own header.
# ---------------------------------------------------------------------------
add_nina_host_test(test_radar_play
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_radar_play.c
)

# ---------------------------------------------------------------------------
# test_night_invert -- neutral-only inversion of the NWS radar tile
# (main/image_night_invert.h), applied once per fetched frame in
# image_page_radar_add() before the frame enters the animation ring.
# Header-only, no ESP-IDF dependency; nina_image_page.c (the only firmware
# caller) pulls in LVGL and the BSP and cannot be host-compiled, which is why
# the pixel classification lives in its own header.
# ---------------------------------------------------------------------------
add_nina_host_test(test_night_invert
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_night_invert.c
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

# ---------------------------------------------------------------------------
# test_radar_wms -- URL/geometry decisions for the road-free radar source
# (main/radar_wms.h): region table, float Web-Mercator, pixel sizing, GetMap
# and GetCapabilities URL shape, and the capabilities TIME-list parser, plus
# radar_site_coords() from main/radar_sites.c which feeds the site box.
# Header-only, no ESP-IDF dependency. The parser also runs over two real
# GetCapabilities documents in test/host/fixtures/ (captured 2026-08-17),
# located at run time through NINA_FIXTURE_DIR.
# ---------------------------------------------------------------------------
add_nina_host_test(test_radar_wms
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_radar_wms.c
        ${NINA_REPO_ROOT}/main/radar_sites.c
)
target_compile_definitions(test_radar_wms PRIVATE
    NINA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

# ---------------------------------------------------------------------------
# test_clouds_wms -- URL/geometry/time decisions for the Clouds page
# (main/clouds_wms.h): East/West satellite pick, square Web-Mercator box from
# lat/lon + zoom, GIBS GetMap and WMTS DescribeDomains URL shape, the
# DescribeDomains time-list parser (start/end/PTnM segments, singles, bare
# dates), the EUMETView GetCapabilities default="..." parser (fixture) and the
# UTC days-from-civil calendar helpers. Header-only, no ESP-IDF dependency.
# ---------------------------------------------------------------------------
add_nina_host_test(test_clouds_wms
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_clouds_wms.c
        # clouds_wms.h derives CLOUDS_PX from screen_size(), which reads
        # g_screen_size out of this file. The default there is 720, so every
        # existing 720-based assertion in this test still holds.
        ${NINA_REPO_ROOT}/main/screen_geom.c
)
target_compile_definitions(test_clouds_wms PRIVATE
    NINA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

# ---------------------------------------------------------------------------
# test_adsb_geom -- pure geometry for the ADS-B page (main/adsb_geom.h):
# curved-earth elevation, haversine distance + initial bearing, angular
# separation on the sky, and the Sky Dome (truncated stereographic) and Radar
# Scope (linear) screen projections. Header-only, only <math.h>, no fixtures.
# The JSON parser in main/adsb_client.c is NOT covered: it pulls in tasks.h ->
# freertos/event_groups.h (no shim) plus poll_task/http_fetch, so it is
# verified on device instead.
# ---------------------------------------------------------------------------
add_nina_host_test(test_adsb_geom
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_adsb_geom.c
)

# ---------------------------------------------------------------------------
# test_image_fit -- the PPA SRM fit geometry for the image pages
# (main/image_fit.h): the n/16 scale pick (round up and centre-crop while the
# crop is under 4 %, otherwise round down and band), the symmetric block trim,
# and the logical->source window mapping for each 0/90/180/270 rotation.
# Header-only, integer-only, no ESP-IDF dependency; the PPA job it feeds lives
# in ui/nina_image_page.c and cannot be host-compiled.
# ---------------------------------------------------------------------------
add_nina_host_test(test_image_fit
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_image_fit.c
)

# ---------------------------------------------------------------------------
# test_clock_dial -- the Classic round clock face's 12-hour forecast dial
# (main/ui/clock_dial.h): 30 degrees per hour, the entry for hour H at the
# clock position of H % 12, each block spanning the hours until the next
# entry (1 on Open-Meteo and Weather Underground, 3 on OpenWeatherMap after
# a short first gap), and nothing past 12 hours from the first entry.
# ---------------------------------------------------------------------------
add_nina_host_test(test_clock_dial
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_clock_dial.c
)

# ---------------------------------------------------------------------------
# test_board_detect -- the RDDID signature table and the controller match
# (main/board_detect.c). Dependency free by design: no ESP-IDF, no FreeRTOS, no
# LVGL, no NVS, so it compiles here directly. Everything that touches hardware
# lives in main/board_profile.c, which is not host compiled.
# ---------------------------------------------------------------------------
add_nina_host_test(test_board_detect
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_board_detect.c
        ${NINA_REPO_ROOT}/main/board_detect.c
)

# ---------------------------------------------------------------------------
# test_screen_geom -- the panel safe area, the inscribed square and the inset
# arithmetic for all three panel rows (main/screen_geom.c), plus the BSP
# backlight curve (components/.../include/bsp/brightness_curve.h, header-only,
# included by relative path so the BSP needs no host include dir).
# screen_geom.h pulls in sdkconfig.h for the family macro, which the host shims
# provide.
# ---------------------------------------------------------------------------
add_nina_host_test(test_screen_geom
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_screen_geom.c
        ${NINA_REPO_ROOT}/main/screen_geom.c
)

# ---------------------------------------------------------------------------
# test_ui_round -- the round layout geometry (main/ui/ui_round.h): rim radius,
# chord widths and the page inset. Header-only and pure, so the only source it
# needs beside the test is main/screen_geom.c, which backs the three globals it
# reads. add_nina_host_test() already puts main/ and main/ui/ on the include
# path and links m for sqrtf.
# ---------------------------------------------------------------------------
add_nina_host_test(test_ui_round
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_ui_round.c
        ${NINA_REPO_ROOT}/main/screen_geom.c
)

# ---------------------------------------------------------------------------
# test_sun_pos -- single-precision solar elevation (main/sun_pos.h): the UTC
# day-of-year split and the NOAA low-accuracy declination / equation-of-time
# elevation used by the Clouds page to pick a visible or infrared layer per
# frame. Header-only, only <math.h>, no fixtures.
# ---------------------------------------------------------------------------
add_nina_host_test(test_sun_pos
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_sun_pos.c
)

# ---------------------------------------------------------------------------
# test_rainviewer -- the pure decisions for the worldwide radar source
# (main/rainviewer.h): zoom/palette validation, the Web-Mercator window, the
# tile range and antimeridian wrap, the tile and GIBS basemap URL shapes, the
# weather-maps.json parse (vendored cJSON) and the provider rule, which needs
# the nearest WSR-88D site from main/radar_sites.c. The header derives its
# window size from screen_size(), so main/screen_geom.c is linked the same way
# test_clouds_wms does it (default 720). The fetch, PNG decode and compositing
# live in main/rainviewer.c, which pulls in http_fetch/stb and is verified on
# device instead.
# ---------------------------------------------------------------------------
add_nina_host_test(test_rainviewer
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_rainviewer.c
        ${NINA_REPO_ROOT}/main/radar_sites.c
        ${NINA_REPO_ROOT}/main/screen_geom.c
)
