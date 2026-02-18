#pragma once

/**
 * @file nina_client_internal.h
 * @brief Internal shared utilities for nina_client modules.
 *
 * Not part of the public API — only included by nina_client.c,
 * nina_api_fetchers.c, nina_sequence.c, and nina_websocket.c.
 */

#include "cJSON.h"
#include <time.h>

/* HTTP GET and parse JSON response. Caller must cJSON_Delete() the result. */
cJSON *http_get_json(const char *url);

/* Parse ISO-8601 datetime string to time_t (UTC). Returns 0 on failure. */
time_t parse_iso8601(const char *str);
