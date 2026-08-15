#pragma once

/**
 * @file octoprint_client.h
 * @brief Read-only OctoPrint REST client for the 3D Printer page.
 *
 * Modeled 1:1 on json_client / ha_client: mutex-protected data struct, a
 * single poll entry point driven by the shared poll_task spine, and every HTTP
 * request through http_fetch. Auth is the OctoPrint "X-Api-Key" header,
 * forwarded via http_fetch_opts_t.extra_header (same path ha_client uses for
 * its Bearer line).
 *
 * READ-ONLY BY CONTRACT: this module issues GET requests only. It never posts
 * a job/printer/connection command. Do not add one here -- a display panel has
 * no business cancelling someone's 14-hour print.
 *
 * Request tiers per poll cycle:
 *   every cycle : GET /api/job            (also the connectivity probe)
 *                 GET /api/printer        (409 = printer off, NOT a failure)
 *                 GET /plugin/DisplayLayerProgress/values   (while available)
 *   every ~30 s : GET /api/connection     (current.state; "Closed" distinguishes
 *                                          printer-off from OctoPrint-down)
 *
 * DisplayLayerProgress is probed implicitly: the values endpoint is requested
 * until it answers 404, which latches dlp_available=false until the next
 * reconnect. No separate probe request.
 *
 * Image pipeline runs only while the page is active (see
 * octoprint_client_set_page_active) and decodes into an RGB565 PSRAM buffer:
 *   source 0 : gcode preview thumbnail (PNG) -- re-fetched only when the
 *              printing file changes. The decoded frame is RETAINED here so a
 *              layout change can re-fit it without a refetch.
 *   source 1 : webcam snapshot (JPEG) -- re-fetched every poll cycle. The UI
 *              (nina_octoprint.c stage_image) CONSUMES the frame under the
 *              mutex once it has scaled it: it frees image_buf and NULLs the
 *              field, since retaining a full-res original that is refetched
 *              next cycle anyway would be pure double-buffering (~600 KB).
 * The buffer is swapped under the mutex and the previous one freed only after
 * unlocking, so a UI reader holding the lock can never observe a freed buffer.
 *
 * Single-owner: only octoprint_poll_task calls octoprint_client_poll(), so the
 * module-static tier/cache state needs no locking. The keep-alive conn slots
 * are the exception: they hold open sockets while parked, so page leave must
 * be able to destroy them from the UI coordinator task; a small mutex in the
 * .c serialises that against a poll in flight.
 */

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define OCTO_STATE_LEN   32   /* "Printing", "Operational", "Offline", ...      */
#define OCTO_CONN_LEN    32   /* /api/connection current.state                  */
#define OCTO_ERROR_LEN   64   /* printer state.error text                       */
#define OCTO_ETA_LEN     16   /* DisplayLayerProgress print.estimatedEndTime    */

/**
 * Live printer state. Every field is written by octoprint_client_poll() under
 * @ref mutex; readers must hold the lock via octoprint_client_lock().
 *
 * "Unknown" sentinels are deliberate and uniform: -1 for the integer fields,
 * NAN for the float temperatures, "" for the text fields. A renderer showing
 * "--" only has to test those.
 */
typedef struct {
    /* -- connectivity ---------------------------------------------------- */
    bool    connected;          /* last /api/job fetch+parse succeeded         */
    bool    data_valid;         /* at least one successful poll since boot     */

    /* -- state ------------------------------------------------------------ */
    char    job_state[OCTO_STATE_LEN];      /* /api/job "state"               */
    char    printer_state[OCTO_STATE_LEN];  /* /api/printer state.text        */
    char    conn_state[OCTO_CONN_LEN];      /* /api/connection current.state; */
                                            /* "Closed" = printer powered off */
                                            /* while OctoPrint itself is up   */
    bool    printing;
    bool    paused;
    bool    error;
    char    error_text[OCTO_ERROR_LEN];     /* "" when no error reported      */

    /* -- job progress ------------------------------------------------------ */
    float   completion;         /* percent 0..100; -1 unknown                  */
    int     print_time_s;       /* elapsed seconds; -1 unknown                 */
    int     print_time_left_s;  /* remaining seconds; -1 unknown               */

    /* -- temperatures (NAN = unknown / printer not operational) ------------ */
    float   nozzle_actual;
    float   nozzle_target;
    float   bed_actual;
    float   bed_target;

    /* -- DisplayLayerProgress plugin --------------------------------------- */
    bool    dlp_available;      /* false once the values endpoint 404s         */
    int     layer_current;      /* -1 unknown                                  */
    int     layer_total;        /* -1 unknown                                  */
    char    eta[OCTO_ETA_LEN];  /* e.g. "20:46"; "" unknown                    */

    /* -- decoded image (RGB565, PSRAM, owned by this module) --------------- */
    uint8_t *image_buf;         /* NULL when no image is held. For the webcam  */
                                /* source the UI frees+NULLs it under the lock */
                                /* after scaling (see file header)             */
    uint16_t image_w;
    uint16_t image_h;
    bool     new_image;         /* set true on buffer swap; the UI clears it   */
                                /* under the lock once it has re-bound the src */
                                /* -- and ONLY then, so a failed alloc/bind    */
                                /* leaves it set and the UI retries next cycle */

    SemaphoreHandle_t mutex;
} octoprint_data_t;

/** Init (creates the mutex, sets the unknown sentinels). Call once before polling. */
void octoprint_client_init(octoprint_data_t *data);

/** Lock the data struct. Returns true if acquired within @p timeout_ms. */
bool octoprint_client_lock(octoprint_data_t *data, int timeout_ms);

/** Unlock the data struct. */
void octoprint_client_unlock(octoprint_data_t *data);

/**
 * Run one poll cycle: job + printer (+ connection on the slow tier, + layer
 * progress while available), then the image fetch when the page is active.
 * Publishes everything into @p data under its mutex.
 *
 * @param base_url      octoprint_url, scheme+host[:port], no trailing path.
 * @param api_key       octoprint_api_key; sent as "X-Api-Key". "" sends none.
 * @param image_source  octoprint_image_source: 0 = gcode thumbnail, 1 = webcam.
 * @param snapshot_url  octoprint_snapshot_url; "" derives
 *                      "<base>/webcam/?action=snapshot".
 *
 * On a /api/job transport or parse failure sets data->connected=false and
 * leaves the previously published values in place (mirrors json_client).
 */
void octoprint_client_poll(const char *base_url, const char *api_key,
                           uint8_t image_source, const char *snapshot_url,
                           octoprint_data_t *data);

/**
 * Gate the image pipeline on page visibility. Passing false frees the decoded
 * buffer immediately (under the mutex) and drops the "which file is cached"
 * memory, so re-entering the page re-fetches. The poll loop itself is gated by
 * the octoprint_page_active flag in tasks.c, matching json/ha.
 */
void octoprint_client_set_page_active(bool active, octoprint_data_t *data);
