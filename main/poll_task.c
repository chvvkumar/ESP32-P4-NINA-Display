/**
 * @file poll_task.c
 * @brief Shared poll-loop skeleton. See poll_task.h for scope.
 */

#include "poll_task.h"
#include "poll_backoff.h"
#include "tasks.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "poll_task";

void poll_loop_run(const poll_loop_spec_t *spec, void *arg) {
    if (!spec || !spec->poll_once || !spec->interval_ms) {
        ESP_LOGE(TAG, "poll_loop_run: invalid spec");
        return;
    }

    if (spec->wifi_group) {
        xEventGroupWaitBits(spec->wifi_group, spec->wifi_bits, pdFALSE, pdFALSE, portMAX_DELAY);
    }
    ESP_LOGI(TAG, "%s: WiFi connected, starting poll loop", spec->name);

    uint32_t backoff_ms = 0; /* 0 = not currently backing off */

    while (1) {
        /* Suspend during OTA updates or while gated inactive (page not visible). */
        while (ota_in_progress || (spec->page_active && !*spec->page_active)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        }

        bool ok = spec->poll_once(arg);

        uint32_t wait_ms;
        if (ok) {
            backoff_ms = 0;
            wait_ms = spec->interval_ms(arg);
        } else if (spec->backoff_initial_ms == 0) {
            /* Backoff disabled -- just retry at the normal interval. */
            wait_ms = spec->interval_ms(arg);
        } else {
            backoff_ms = poll_backoff_next(backoff_ms, spec->backoff_initial_ms, spec->backoff_max_ms);
            wait_ms = backoff_ms;
            ESP_LOGW(TAG, "%s: poll failed, retrying in %u ms", spec->name, (unsigned)wait_ms);
        }

        /* A pending task notify (e.g. WebSocket wake, config change) wakes
         * this early instead of sleeping the full interval. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}
