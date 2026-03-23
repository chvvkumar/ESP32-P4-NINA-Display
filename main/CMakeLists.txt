set(LV_DEMO_DIR ../managed_components/lvgl__lvgl/demos)
file(GLOB_RECURSE LV_DEMOS_SOURCES ${LV_DEMO_DIR}/*.c)

idf_component_register(
    SRCS main.c tasks.c power_mgmt.c jpeg_utils.c stb_image.c perf_monitor.c ota_github.c
         nina_connection.c nina_client.c nina_api_fetchers.c nina_sequence.c nina_websocket.c
         allsky_client.c weather_client.c spotify_auth.c spotify_client.c demo_data.c
         app_config.c web_server.c web_handlers_config.c web_handlers_display.c web_handlers_system.c web_handlers_allsky.c web_handlers_spotify.c mqtt_ha.c
         ui/nina_dashboard.c ui/nina_dashboard_update.c ui/nina_thumbnail.c ui/nina_graph_overlay.c ui/nina_graph_controls.c ui/nina_sysinfo.c ui/nina_summary.c ui/nina_allsky.c ui/nina_clock.c ui/nina_spotify.c ui/nina_settings_tabview.c ui/settings_tab_display.c ui/settings_tab_nodes.c ui/settings_tab_behavior.c ui/settings_tab_system.c ui/settings_color_picker.c ui/themes.c ui/ui_styles.c
         ui/nina_toast.c ui/nina_event_log.c ui/nina_alerts.c ui/nina_safety.c ui/nina_session_stats.c ui/lv_font_material_safety.c ui/lv_font_superscript_24.c ui/lv_font_playfair_228.c ui/lv_font_playfair_90.c ui/lv_font_overpass_27.c ui/lv_font_overpass_16.c
         ui/nina_info_overlay.c ui/nina_info_camera.c ui/nina_info_mount.c
         ui/nina_info_imagestats.c ui/nina_info_sequence.c ui/nina_info_filter.c ui/nina_info_autofocus.c ui/nina_info_session_stats.c
         ui/nina_ota_prompt.c
         ${LV_DEMOS_SOURCES}
    INCLUDE_DIRS . ui ${LV_DEMO_DIR}
    EMBED_TXTFILES config_ui.html
    EMBED_FILES favicon.png logo.jpg spotify_logo.png)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB}
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -Wno-attributes      # conflicting section attrs in lv_color/lv_math (upstream LVGL issue)
        -Wno-unused-function  # unused atomic.h inlines in lv_freertos.c (upstream ESP-IDF issue)
)

# Add hardware JPEG decoder for thumbnail feature
idf_component_get_property(JPEG_LIB esp_driver_jpeg COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} PRIVATE ${JPEG_LIB})

# Build-time generated version header (git tag, sha, branch)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(${COMPONENT_LIB} generate_build_version)
