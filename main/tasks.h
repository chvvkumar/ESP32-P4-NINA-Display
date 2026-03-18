#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nina_client.h"
#include "app_config.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define WIFI_CONNECTED_BIT BIT0

/* Shared state — defined in main.c, used by tasks.c */
extern EventGroupHandle_t s_wifi_event_group;
extern int instance_count;

/* Signals the data task that a page switch occurred — defined in tasks.c */
extern _Atomic bool page_changed;

/** Task handle for data_update_task (UI coordinator) — used by WebSocket to wake for immediate UI refresh. */
extern TaskHandle_t data_task_handle;

/** Per-instance poll task handles — used by WebSocket to wake poll tasks for immediate re-poll. */
extern TaskHandle_t poll_task_handles[MAX_NINA_INSTANCES];

/** OTA in progress — data task suspends when true. Set by OTA handler. */
extern _Atomic bool ota_in_progress;

/** On-demand firmware update check — set by settings page button. */
extern _Atomic bool ota_check_requested;

/** Screen touch detected — wakes display from screen sleep. Set by LVGL touch handler. */
extern _Atomic bool screen_touch_wake;

/** True while the display is in screen-sleep mode (backlight off). */
extern _Atomic bool screen_asleep;

/* ── Per-instance poll task context ── */
typedef struct {
    int               index;            /* Instance index (0..MAX_NINA_INSTANCES-1) */
    nina_client_t    *client;           /* Pointer to shared instances[index] */
    nina_poll_state_t *poll_state;      /* Owned by this task (not shared) */
    TaskHandle_t      task_handle;      /* For task notifications */
    volatile bool     is_active;        /* true = foreground (full poll), false = background */
    volatile bool     shutdown;         /* true = task should exit */
    bool              filters_synced;   /* Per-instance filter sync flag */
    int64_t           last_heartbeat_ms; /* For background polling interval */
} instance_poll_ctx_t;

/** Page-change callback registered with the dashboard swipe gesture. */
void on_page_changed(int new_page);

/** FreeRTOS task: handles BOOT button via GPIO ISR — page cycling, screen wake, long-press deep sleep. */
void input_task(void *arg);

/** FreeRTOS task: per-instance NINA data poller (one per configured instance). */
void instance_poll_task(void *arg);

/** FreeRTOS task: AllSky API poller — polls at allsky_update_interval_s rate. */
void allsky_poll_task(void *arg);

/** Spotify poll task handle and page-active flag — defined in tasks.c. */
extern TaskHandle_t spotify_task_handle;
extern _Atomic bool spotify_page_active;

/** FreeRTOS task: Spotify API poller — fetches currently-playing, album art on track change. */
void spotify_poll_task(void *arg);

/** FreeRTOS task: UI coordinator — reads cached data, updates LVGL, handles auto-rotate. */
void data_update_task(void *arg);

/* ── Async fetch worker (offloads HTTP fetches from Core 1 to Core 0) ── */

/** Fetch request types */
typedef enum {
    FETCH_THUMBNAIL,
    FETCH_GRAPH_RMS,
    FETCH_GRAPH_HFR,
    FETCH_GRAPH_HFR_RING,   /* Build from local ring buffer (no HTTP) */
    FETCH_INFO_CAMERA,
    FETCH_INFO_MOUNT,
    FETCH_INFO_SEQUENCE,
    FETCH_INFO_FILTER,
    FETCH_INFO_IMAGESTATS,
    FETCH_INFO_AUTOFOCUS,
} fetch_type_t;

/** Fetch request — posted to s_fetch_queue by data_update_task */
typedef struct {
    fetch_type_t type;
    int          instance_idx;     /* NINA instance index */
    char         url[128];         /* API base URL */
    int          max_points;       /* For graph requests */
    nina_client_t *client;         /* For ring buffer reads (FETCH_GRAPH_HFR_RING) */
} fetch_request_t;

/** Fetch result — posted to s_fetch_result_queue by fetch worker */
typedef struct {
    fetch_type_t type;
    int          instance_idx;
    bool         success;
    union {
        struct {
            uint8_t *rgb565_data;
            uint32_t w, h, data_size;
        } thumbnail;
        void *data;   /* graph_rms_data_t*, graph_hfr_data_t*, or overlay data */
    };
} fetch_result_t;

/** FreeRTOS task: async fetch worker — runs HTTP fetches on Core 0. */
void fetch_worker_task(void *arg);
