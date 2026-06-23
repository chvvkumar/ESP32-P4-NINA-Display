/**
 * @file web_handlers_logs.c
 * @brief Web endpoints for the boot log capture buffer and persistent crash log.
 *
 * GET  /api/logs            -> streams the captured log oldest->newest as a
 *                              text/plain attachment, chunked from PSRAM.
 * POST /api/logs/clear      -> empties the capture buffer, returns {"ok":true}.
 * GET  /api/crashlog        -> streams the persistent crash-history JSONL file
 *                              (text/plain attachment; the UI also fetches it
 *                              inline). Empty body when no crashes recorded.
 * POST /api/crashlog/clear  -> deletes the crash-history file, returns {"ok":true}.
 * GET  /api/coredump/info   -> JSON {present,size,git_sha,version} for the saved
 *                              ELF core dump in the coredump partition.
 * GET  /api/coredump        -> streams the raw core dump ELF as an octet-stream
 *                              attachment (404 when no dump is present).
 * POST /api/coredump/clear  -> erases the saved core dump, returns {"ok":true}.
 */

#include "web_server_internal.h"
#include "log_capture.h"
#include "crash_log.h"
#include "build_version.h"
#include "esp_heap_caps.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
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

// Handler for downloading / displaying the persistent crash-history file
esp_err_t crashlog_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    /* Build the download filename from the configured hostname. */
    const char *hostname = app_config_get()->hostname;
    if (!hostname || hostname[0] == '\0') {
        hostname = "ninadash";
    }
    char disp[96];
    snprintf(disp, sizeof(disp),
             "attachment; filename=\"%s-crashlog.jsonl\"", hostname);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    /* No crash history yet (or SPIFFS unmounted): respond 200 with empty body so
     * the UI can render an "no crashes" state without treating it as an error. */
    FILE *f = crash_log_open_read();
    if (!f) {
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    char *chunk = heap_caps_malloc(LOG_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!chunk) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Stream the JSONL file start->end in PSRAM-sized chunks. */
    for (;;) {
        size_t got = fread(chunk, 1, LOG_CHUNK_SIZE, f);
        if (got > 0) {
            if (httpd_resp_send_chunk(req, chunk, got) != ESP_OK) {
                heap_caps_free(chunk);
                fclose(f);
                return ESP_FAIL;  /* connection aborted; httpd cleans up */
            }
        }
        if (got < LOG_CHUNK_SIZE) {
            break;  /* EOF or short read */
        }
    }

    heap_caps_free(chunk);
    fclose(f);

    /* Terminate the chunked response with a zero-length chunk. */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// Handler for clearing the persistent crash-history file
esp_err_t crashlog_clear_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    crash_log_clear();  /* idempotent, always ESP_OK */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler reporting whether a saved core dump is present (+ its size and build info)
esp_err_t coredump_info_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    /* esp_core_dump_image_get() returns ESP_OK only when a valid ELF image is
     * present in the coredump partition; out_size is the image length. */
    size_t out_addr = 0, out_size = 0;
    bool present = (esp_core_dump_image_get(&out_addr, &out_size) == ESP_OK);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "present", present);
    cJSON_AddNumberToObject(root, "size", present ? (double)out_size : 0.0);
    cJSON_AddStringToObject(root, "git_sha", BUILD_GIT_SHA);
    cJSON_AddStringToObject(root, "version", BUILD_VERSION);

    const char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free((void *)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// Handler for downloading the raw core dump ELF from the coredump partition
esp_err_t coredump_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    /* No core dump saved: 404 so the UI can distinguish "nothing to download". */
    size_t out_addr = 0, out_size = 0;
    if (esp_core_dump_image_get(&out_addr, &out_size) != ESP_OK || out_size == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "No core dump present", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Build the download filename from the configured hostname + build SHA. */
    const char *hostname = app_config_get()->hostname;
    if (!hostname || hostname[0] == '\0') {
        hostname = "ninadash";
    }
    char disp[160];
    snprintf(disp, sizeof(disp),
             "attachment; filename=\"%s-%s-coredump.elf\"", hostname, BUILD_GIT_SHA);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char *chunk = heap_caps_malloc(LOG_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!chunk) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Stream exactly out_size bytes from the partition in PSRAM-sized chunks. */
    size_t remaining = out_size;
    size_t offset = 0;
    while (remaining > 0) {
        size_t to_read = remaining < LOG_CHUNK_SIZE ? remaining : (size_t)LOG_CHUNK_SIZE;
        esp_err_t rerr = esp_partition_read(part, offset, chunk, to_read);
        if (rerr != ESP_OK) {
            heap_caps_free(chunk);
            return ESP_FAIL;  /* partial response; httpd aborts the connection */
        }
        if (httpd_resp_send_chunk(req, chunk, to_read) != ESP_OK) {
            heap_caps_free(chunk);
            return ESP_FAIL;  /* connection aborted; httpd cleans up */
        }
        offset    += to_read;
        remaining -= to_read;
    }

    heap_caps_free(chunk);

    /* Terminate the chunked response with a zero-length chunk. */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// Handler for erasing the saved core dump
esp_err_t coredump_clear_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
