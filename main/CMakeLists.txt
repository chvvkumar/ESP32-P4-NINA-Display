set(LV_DEMO_DIR ../managed_components/lvgl__lvgl/demos)
file(GLOB_RECURSE LV_DEMOS_SOURCES ${LV_DEMO_DIR}/*.c)

idf_component_register(
    SRCS main.c tasks.c jpeg_utils.c
         nina_client.c nina_api_fetchers.c nina_sequence.c nina_websocket.c
         app_config.c web_server.c mqtt_ha.c
         ui/nina_dashboard.c ui/nina_dashboard_update.c ui/nina_thumbnail.c ui/themes.c
         ${LV_DEMOS_SOURCES}
    INCLUDE_DIRS . ui ${LV_DEMO_DIR}
    EMBED_TXTFILES config_ui.html)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB}
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)

# Add hardware JPEG decoder for thumbnail feature
idf_component_get_property(JPEG_LIB esp_driver_jpeg COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} PRIVATE ${JPEG_LIB})
