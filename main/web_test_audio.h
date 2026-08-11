#pragma once

/**
 * @file web_test_audio.h
 * @brief Standalone speaker/voice test server on port 8080.
 *
 * A second esp_http_server instance, deliberately isolated from the main web
 * server: three routes, no auth, and the only thing reachable through it is
 * clip playback.  Not linked from the main UI -- browse to
 * http://<device>:8080/ directly.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Start the test server.  Call once, after the main web server is up. */
void web_test_audio_init(void);

#ifdef __cplusplus
}
#endif
