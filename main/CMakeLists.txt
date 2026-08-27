set(LV_DEMO_DIR ../managed_components/lvgl__lvgl/demos)
file(GLOB_RECURSE LV_DEMOS_SOURCES ${LV_DEMO_DIR}/*.c)

idf_component_register(
    SRCS main.c tasks.c axi_qos.c power_mgmt.c jpeg_utils.c stb_image.c image_red_remap.c perf_monitor.c ota_github.c
         http_fetch.c poll_task.c image_page_poll.c radar_sites.c time_parse.c wifi_join.c
         board_detect.c board_profile.c screen_geom.c
         nina_connection.c nina_client.c nina_api_fetchers.c nina_sequence.c nina_websocket.c
         net_diag.c net_trace.c allsky_client.c json_client.c ha_client.c adsb_client.c octoprint_client.c octoprint_appkeys.c goes_client.c weather_client.c telemetry.c moon_ephemeris.c moon_sphere.cpp moon_interaction.c spotify_auth.c spotify_client.c demo_data.c
         audio_alert.c web_test_audio.c voice_store.c web_handlers_voice.c
         app_config.c settings_table.c control_registry.c web_server.c web_route_auth.c web_handlers_config.c web_handlers_display.c web_handlers_system.c web_handlers_allsky.c web_handlers_json.c web_handlers_ha.c web_handlers_octoprint.c web_handlers_spotify.c web_handlers_image_display.c web_handlers_pages.c web_handlers_control.c web_handlers_auth.c web_handlers_wifi.c web_handlers_logs.c log_capture.c crash_log.c mqtt_ha.c
         ui/page_registry.c ui/nina_dashboard.c ui/nina_dashboard_update.c ui/nina_nav_arbiter.c ui/nina_thumbnail.c ui/nina_graph_overlay.c ui/graph_downsample.c ui/nina_graph_controls.c ui/nina_sysinfo.c ui/nina_summary.c ui/nina_allsky.c ui/nina_tile_grid.c ui/nina_json.c ui/nina_ha.c ui/nina_adsb.c ui/nina_octoprint.c ui/octoprint_layout_bento.c ui/octoprint_layout_glass.c ui/octoprint_layout_overlay.c ui/octoprint_layout_letterbox.c ui/nina_image_page.c ui/nina_subbar.c ui/nina_layout_image.c ui/nina_clock.c ui/nina_spotify.c ui/settings_hub.c ui/settings_wifi.c ui/themes.c ui/ui_styles.c ui/ui_helpers.c
         ui/nina_toast.c ui/nina_event_log.c ui/nina_alerts.c ui/nina_safety.c ui/nina_session_stats.c ui/lv_font_material_safety.c ui/lv_font_material_icons_idle.c ui/lv_font_warning_128.c ui/lv_font_superscript_24.c ui/lv_font_playfair_228.c ui/lv_font_playfair_90.c ui/lv_font_overpass_27.c ui/lv_font_overpass_16.c ui/lv_font_montserrat_64.c ui/lv_font_montserrat_bold_22.c ui/lv_font_montserrat_bold_28.c ui/lv_font_montserrat_bold_56.c ui/lv_font_saira_black_300.c ui/lv_font_saira_black_110.c ui/lv_font_saira_black_66.c ui/lv_font_saira_thin_240.c ui/lv_font_saira_thin_84.c ui/lv_font_saira_light_46.c ui/lv_font_bodoni_black_150.c ui/lv_font_bodoni_ital_90.c ui/lv_font_bodoni_ital_44.c ui/lv_font_stencil_190.c ui/lv_font_hanken_black_96.c ui/lv_font_hanken_bold_44.c ui/lv_font_hanken_bold_64.c ui/lv_font_hanken_bold_28.c ui/lv_font_hanken_black_40.c
         ui/nina_setup_screen.c ui/nina_setup_hint.c ui/nina_idle_indicator.c
         ui/nina_info_overlay.c ui/nina_net_debug.c ui/nina_info_camera.c ui/nina_info_mount.c
         ui/nina_info_imagestats.c ui/nina_info_sequence.c ui/nina_info_filter.c ui/nina_info_autofocus.c ui/nina_info_session_stats.c
         ui/nina_ota_prompt.c
         ui/nina_wait_overlay.c
         ui/nina_empty_state.c
         ${LV_DEMOS_SOURCES}
    INCLUDE_DIRS . ui ${LV_DEMO_DIR}
    # The 21 fragment_*.html tabs are embedded ONLY as their .gz twins (see
    # WEB_GZ_ASSETS below): the plain copies cost ~200 KB of flash and every
    # client that fetches a tab sends Accept-Encoding: gzip. Their
    # _binary_fragment_*_html_start/_end symbols therefore no longer exist --
    # send_embedded_asset() serves the gz copy unconditionally for these.
    # config_ui.html / home_ui.html keep both copies; login.html and
    # setup_ui.html are plain-only (served before any UI state exists).
    EMBED_TXTFILES config_ui.html home_ui.html login.html setup_ui.html
                   # Standalone audio test page (web_test_audio.c, port 8080).
                   # Deliberately NOT in WEB_GZ_ASSETS: one small page served by
                   # a second httpd instance that has no gzip negotiation.
                   test_audio.html
    EMBED_FILES favicon.png logo.jpg moon_equirect.jpg
                # Voice-alert clips: headerless 16 kHz/16-bit/mono PCM. The embed
                # symbol is derived from the basename only, so audio_alert.c refers
                # to them as _binary_<name>_pcm_start/_end. Regenerate placeholders
                # with tools/gen_placeholder_clips.py.
                audio_clips/chime.pcm audio_clips/warning.pcm
                audio_clips/instance_1.pcm audio_clips/instance_2.pcm audio_clips/instance_3.pcm
                audio_clips/rms.pcm audio_clips/hfr.pcm
                audio_clips/above_limit.pcm audio_clips/rms_above.pcm audio_clips/hfr_above.pcm
                audio_clips/unsafe.pcm audio_clips/point.pcm
                audio_clips/digit_0.pcm audio_clips/digit_1.pcm audio_clips/digit_2.pcm
                audio_clips/digit_3.pcm audio_clips/digit_4.pcm audio_clips/digit_5.pcm
                audio_clips/digit_6.pcm audio_clips/digit_7.pcm audio_clips/digit_8.pcm
                audio_clips/digit_9.pcm
                audio_clips/connected.pcm audio_clips/disconnected.pcm audio_clips/and.pcm
                audio_clips/camera.pcm audio_clips/mount.pcm audio_clips/guider.pcm
                audio_clips/focuser.pcm audio_clips/filterwheel.pcm audio_clips/rotator.pcm
                audio_clips/safety.pcm audio_clips/dome.pcm audio_clips/flat.pcm
                audio_clips/switch.pcm audio_clips/weather.pcm
                audio_clips/sequence_event.pcm audio_clips/focuser_event.pcm
                audio_clips/mount_event.pcm audio_clips/meridian_flip.pcm
                audio_clips/guider_event.pcm audio_clips/safety_event.pcm
                audio_clips/error_event.pcm audio_clips/profile_changed.pcm
                audio_clips/dome_event.pcm audio_clips/flat_event.pcm
                audio_clips/boot_jingle.pcm
                # Per-event phrase clips (voice_event_t in audio_alert.h).
                # 15 events, 13 files: VOICE_EV_MERIDIAN_FLIP_COMPLETE reuses
                # meridian_flip.pcm above, and VOICE_EV_CONDITIONS_UNSAFE is
                # reserved with no clip and no producer (the breach engine's
                # unsafe.pcm already announces that edge). Reviving it means
                # re-rendering conditions_unsafe.pcm from tools/gen_voice_clips.py
                # AND adding a producer.
                audio_clips/sequence_started.pcm audio_clips/sequence_finished.pcm
                audio_clips/sequence_step_failed.pcm
                audio_clips/autofocus_complete.pcm audio_clips/autofocus_failed.pcm
                audio_clips/meridian_flip_starting.pcm audio_clips/mount_parked.pcm
                audio_clips/conditions_safe.pcm
                audio_clips/guiding_stopped.pcm
                audio_clips/dome_shutter_closed.pcm audio_clips/dome_shutter_opened.pcm
                audio_clips/platesolve_failed.pcm audio_clips/camera_timeout.pcm)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB}
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -Wno-attributes      # conflicting section attrs in lv_color/lv_math (upstream LVGL issue)
        -Wno-unused-function  # unused atomic.h inlines in lv_freertos.c (upstream ESP-IDF issue)
)

# Custom LVGL malloc backend (routes lv_malloc -> PSRAM). Must be compiled INTO the
# LVGL library, not main: lv_mem.c (inside liblvgl) references lv_*_core, and liblvgl
# is linked after libmain. A single-pass linker would not extract these symbols from
# libmain.a (nothing needs them yet when main is processed), leaving them undefined.
# Building it into liblvgl resolves the reference within the same archive.
target_sources(${LVGL_LIB} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lv_mem_psram.c)

# Add hardware JPEG decoder for thumbnail feature
idf_component_get_property(JPEG_LIB esp_driver_jpeg COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} PRIVATE ${JPEG_LIB})

# Add tgx software 3D renderer (moon sphere). Linked like the JPEG lib above so
# main keeps its implicit dependency on all components (a PRIV_REQUIRES/REQUIRES
# line here would disable that and drop driver/ppa.h, esp_http_server.h, etc.).
idf_component_get_property(TGX_LIB tgx COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} PRIVATE ${TGX_LIB})

# NOTE: no -Wl,--wrap=panic_print_char here. Every caller of panic_print_char()
# is inside esp_system/panic.c, the TU that defines it, and --wrap only rewrites
# cross-TU references — the wrapper was linked but never reached. crash_log.c now
# reads the panic detail text back from the core dump instead.

# Build-time generated version header (git tag, sha, branch)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(${COMPONENT_LIB} generate_build_version)

# Build-time gzip of the embedded web UI. config_ui.html and home_ui.html are
# embedded twice: the plain copy via EMBED_TXTFILES above (symbols unchanged)
# and a .gz copy generated below; send_embedded_asset() in
# web_handlers_config.c picks between them per request from Accept-Encoding.
# The fragment_*.html tabs are embedded ONLY here, as .gz -- they have no
# plain copy and no _binary_fragment_*_html_start symbol.
# login.html / setup_ui.html are deliberately excluded: both are served on
# paths that must work before any UI state exists, and neither is large enough
# to pay for the extra flash.
set(WEB_GZ_ASSETS
    config_ui.html home_ui.html
    fragment_logs.html fragment_backup.html fragment_api.html
    fragment_allsky.html fragment_json.html fragment_ha.html
    fragment_clock.html fragment_spotify.html
    fragment_image_goes.html fragment_image_moon.html
    fragment_image_solar.html fragment_image_custom.html
    fragment_image_radar.html
    fragment_image_clouds.html
    fragment_octoprint.html
    fragment_adsb.html
    fragment_nodes.html fragment_display.html fragment_behavior.html
    fragment_system.html fragment_voice_clips.html)

idf_build_get_property(python PYTHON)
idf_build_get_property(project_dir PROJECT_DIR)
set(web_gz_dir ${CMAKE_BINARY_DIR}/generated/gz)
set(web_gz_files "")
foreach(asset IN LISTS WEB_GZ_ASSETS)
    # Plain DEPENDS on the .html, not MAIN_DEPENDENCY: EMBED_TXTFILES already
    # claims each .html as the MAIN_DEPENDENCY of its own embed rule, and a
    # source file may only be the main dependency of one custom command.
    add_custom_command(
        OUTPUT ${web_gz_dir}/${asset}.gz
        COMMAND ${python} ${project_dir}/tools/gzip_asset.py
                ${CMAKE_CURRENT_SOURCE_DIR}/${asset}
                ${web_gz_dir}/${asset}.gz
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${asset}
                ${project_dir}/tools/gzip_asset.py
        COMMENT "Gzipping ${asset}"
        VERBATIM)
    list(APPEND web_gz_files ${web_gz_dir}/${asset}.gz)
endforeach()

add_custom_target(generate_web_gz DEPENDS ${web_gz_files})
add_dependencies(${COMPONENT_LIB} generate_web_gz)

# target_add_binary_data takes each .gz as the MAIN_DEPENDENCY of its .S
# generation, so editing an .html regenerates the .gz and then the .S. It is
# deliberately NOT given DEPENDS generate_web_gz: a target-level dependency
# there would make the (slow) hex conversion of config_ui.html.gz re-run on
# every build, since generate_web_gz is always considered out of date.
foreach(asset IN LISTS WEB_GZ_ASSETS)
    target_add_binary_data(${COMPONENT_LIB} "${web_gz_dir}/${asset}.gz" BINARY)
endforeach()
