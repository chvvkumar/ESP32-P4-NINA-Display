set(LV_DEMO_DIR ../managed_components/lvgl__lvgl/demos)
file(GLOB_RECURSE LV_DEMOS_SOURCES ${LV_DEMO_DIR}/*.c)

idf_component_register(
    SRCS main.c tasks.c jpeg_utils.c perf_monitor.c
         nina_connection.c nina_client.c nina_api_fetchers.c nina_sequence.c nina_websocket.c
         app_config.c web_server.c web_handlers_config.c web_handlers_display.c web_handlers_system.c mqtt_ha.c
         ui/nina_dashboard.c ui/nina_dashboard_update.c ui/nina_thumbnail.c ui/nina_graph_overlay.c ui/nina_graph_controls.c ui/nina_sysinfo.c ui/nina_summary.c ui/nina_settings.c ui/themes.c ui/ui_styles.c
         ui/nina_info_overlay.c ui/nina_info_camera.c ui/nina_info_mount.c
         ui/nina_info_imagestats.c ui/nina_info_sequence.c ui/nina_info_filter.c ui/nina_info_autofocus.c
         ${LV_DEMOS_SOURCES}
    INCLUDE_DIRS . ui ${LV_DEMO_DIR}
    EMBED_TXTFILES config_ui.html)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB}
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
        -Wno-attributes      # conflicting section attrs in lv_color/lv_math (upstream LVGL issue)
        -Wno-unused-function  # unused atomic.h inlines in lv_freertos.c (upstream ESP-IDF issue)
)

# Add hardware JPEG decoder for thumbnail feature
idf_component_get_property(JPEG_LIB esp_driver_jpeg COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} PRIVATE ${JPEG_LIB})

# Inject git metadata as compile definitions
if(GIT_TAG)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BUILD_GIT_TAG="${GIT_TAG}")
else()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BUILD_GIT_TAG="unknown")
endif()

if(GIT_SHA)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BUILD_GIT_SHA="${GIT_SHA}")
else()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BUILD_GIT_SHA="unknown")
endif()

if(GIT_BRANCH)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BUILD_GIT_BRANCH="${GIT_BRANCH}")
else()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BUILD_GIT_BRANCH="unknown")
endif()
