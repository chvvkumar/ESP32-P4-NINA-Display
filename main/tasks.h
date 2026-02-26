#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <stdbool.h>

#define WIFI_CONNECTED_BIT BIT0

/* Shared state — defined in main.c, used by tasks.c */
extern EventGroupHandle_t s_wifi_event_group;
extern int instance_count;

/* Signals the data task that a page switch occurred — defined in tasks.c */
extern volatile bool page_changed;

/** Task handle for data_update_task — used by WebSocket to wake the task early. */
extern TaskHandle_t data_task_handle;

/** OTA in progress — data task suspends when true. Set by OTA handler. */
extern volatile bool ota_in_progress;

/** Screen touch detected — wakes display from screen sleep. Set by LVGL touch handler. */
extern volatile bool screen_touch_wake;

/** True while the display is in screen-sleep mode (backlight off). */
extern volatile bool screen_asleep;

/** Page-change callback registered with the dashboard swipe gesture. */
void on_page_changed(int new_page);

/** FreeRTOS task: polls BOOT button to switch pages. */
void input_task(void *arg);

/** FreeRTOS task: polls all NINA instances and updates the dashboard. */
void data_update_task(void *arg);
