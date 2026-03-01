#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nina_client.h"
#include "app_config.h"
#include <stdbool.h>
#include <stdint.h>

#define WIFI_CONNECTED_BIT BIT0

/* Shared state — defined in main.c, used by tasks.c */
extern EventGroupHandle_t s_wifi_event_group;
extern int instance_count;

/* Signals the data task that a page switch occurred — defined in tasks.c */
extern volatile bool page_changed;

/** Task handle for data_update_task (UI coordinator) — used by WebSocket to wake for immediate UI refresh. */
extern TaskHandle_t data_task_handle;

/** Per-instance poll task handles — used by WebSocket to wake poll tasks for immediate re-poll. */
extern TaskHandle_t poll_task_handles[MAX_NINA_INSTANCES];

/** OTA in progress — data task suspends when true. Set by OTA handler. */
extern volatile bool ota_in_progress;

/** On-demand firmware update check — set by settings page button. */
extern volatile bool ota_check_requested;

/** Screen touch detected — wakes display from screen sleep. Set by LVGL touch handler. */
extern volatile bool screen_touch_wake;

/** True while the display is in screen-sleep mode (backlight off). */
extern volatile bool screen_asleep;

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

/** FreeRTOS task: UI coordinator — reads cached data, updates LVGL, handles auto-rotate. */
void data_update_task(void *arg);
