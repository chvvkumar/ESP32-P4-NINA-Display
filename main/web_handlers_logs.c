/**
 * @file web_handlers_logs.c
 * @brief Web endpoints for the boot log capture buffer.
 *
 * GET  /api/logs        -> streams the captured log oldest->newest as a
 *                          text/plain attachment, chunked from PSRAM.
 * POST /api/logs/clear  -> empties the capture buffer, returns {"ok":true}.
 */

#include "web_server_internal.h"
#include "log_capture.h"
#include "esp_heap_caps.h"
#include <string.h>

/* PSRAM-backed chunk size for streaming the download. Keeps the send path off
 * the tight internal heap. */
#define LOG_CHUNK_SIZE 4096

// Handler for downloading the captured log buffer
esp_err_t logs_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    /* Build the download filename from the configured hostname. */
    const char *hostname = app_config_get()->hostname;
    if (!hostname || hostname[0] == '\0') {
        hostname = "ninadash";
    }
    char disp[96];
    snprintf(disp, sizeof(disp),
             "attachment; filename=\"%s-logs.txt\"", hostname);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char *chunk = heap_caps_malloc(LOG_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!chunk) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Stream oldest->newest. Each read locks the ring only for its own copy,
     * so logging is never blocked across the whole HTTP send. New lines that
     * arrive mid-download simply extend what later reads return. */
    size_t offset = 0;
    for (;;) {
        size_t got = log_capture_read(offset, chunk, LOG_CHUNK_SIZE);
        if (got == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, chunk, got) != ESP_OK) {
            heap_caps_free(chunk);
            return ESP_FAIL;  /* connection aborted; httpd cleans up */
        }
        offset += got;
    }

    heap_caps_free(chunk);

    /* Terminate the chunked response with a zero-length chunk. */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// Handler for clearing the captured log buffer
esp_err_t logs_clear_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    log_capture_clear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
