#pragma once

/**
 * @file nina_api_fetchers.h
 * @brief Individual REST API endpoint fetch functions for NINA.
 *
 * Internal header — only included by nina_client.c.
 */

#include "nina_client.h"

void fetch_camera_info_robust(const char *base_url, nina_client_t *data);
void fetch_filter_robust_ex(const char *base_url, nina_client_t *data, bool fetch_available);
int  fetch_image_count(const char *base_url);
void fetch_image_history_robust(const char *base_url, nina_client_t *data);
void fetch_profile_robust(const char *base_url, nina_client_t *data);
void fetch_guider_robust(const char *base_url, nina_client_t *data);
void fetch_mount_robust(const char *base_url, nina_client_t *data);
void fetch_focuser_robust(const char *base_url, nina_client_t *data);
void fetch_switch_info(const char *base_url, nina_client_t *data);
void fetch_safety_monitor_info(const char *base_url, nina_client_t *data);

/**
 * @brief Fetch all equipment info from the bundled /equipment/info endpoint (ninaAPI 2.2.15+).
 * Populates camera, filter wheel, focuser, guider, mount, switch, and safety monitor
 * fields in nina_client_t from a single HTTP request.
 *
 * @param base_url          NINA API base URL
 * @param data              Client data structure to populate
 * @param fetch_filter_list If true, also parse AvailableFilters[] (use on first connect)
 * @return 0 on success, -1 on HTTP failure (offline), -2 if endpoint unavailable (404/error)
 */
int fetch_equipment_info_bundled(const char *base_url, nina_client_t *data, bool fetch_filter_list);

/* Info overlay detail fetchers — on-demand, not part of normal polling */
#include "ui/info_overlay_types.h"
void fetch_camera_details(const char *base_url, camera_detail_data_t *out);
void fetch_weather_details(const char *base_url, camera_detail_data_t *out);
void fetch_mount_details(const char *base_url, mount_detail_data_t *out);
void fetch_sequence_details(const char *base_url, sequence_detail_data_t *out);

/* Graph data fetchers — used by graph overlay, not part of normal polling */
#include "graph_data_types.h"
void fetch_guider_graph(const char *base_url, graph_rms_data_t *out, int max_points);
void fetch_hfr_history(const char *base_url, graph_hfr_data_t *out, int max_points);
void build_hfr_from_ring(const nina_client_t *client, graph_hfr_data_t *out, int max_points);
