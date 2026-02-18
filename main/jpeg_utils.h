#pragma once

#include <stdbool.h>

/**
 * @brief Fetch a prepared JPEG image from NINA, hardware-decode it, and display it as a thumbnail.
 * @param base_url NINA API base URL for the instance
 * @return true if image was successfully fetched, decoded, and handed to the dashboard
 */
bool fetch_and_show_thumbnail(const char *base_url);
