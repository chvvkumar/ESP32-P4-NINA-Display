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
void fetch_image_history_robust(const char *base_url, nina_client_t *data);
void fetch_profile_robust(const char *base_url, nina_client_t *data);
void fetch_guider_robust(const char *base_url, nina_client_t *data);
void fetch_mount_robust(const char *base_url, nina_client_t *data);
void fetch_focuser_robust(const char *base_url, nina_client_t *data);
void fetch_switch_info(const char *base_url, nina_client_t *data);
