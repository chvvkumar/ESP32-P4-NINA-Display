#pragma once

#include "esp_err.h"
#include "esp_sleep.h"
#include <stdint.h>

/**
 * Initialize power management — enables Dynamic Frequency Scaling.
 * Must be called after app_config_init() and before task creation.
 * Requires CONFIG_PM_ENABLE=y in sdkconfig.
 */
esp_err_t power_mgmt_init(void);

/**
 * Enter deep sleep with timer wake source from config.
 * Saves state to RTC memory before sleeping.
 * This function does NOT return — the chip reboots on wake.
 * Caller should stop LVGL, turn off backlight, and show toast before calling.
 */
void power_mgmt_enter_deep_sleep(uint32_t wake_timer_s, int current_page);

/**
 * Check if we woke from deep sleep.
 * Call early in app_main() after NVS init.
 * Returns the wake cause (ESP_SLEEP_WAKEUP_UNDEFINED if normal boot).
 */
esp_sleep_wakeup_cause_t power_mgmt_check_wake_cause(void);

/**
 * Get the page index saved before deep sleep.
 * Only meaningful when power_mgmt_check_wake_cause() != ESP_SLEEP_WAKEUP_UNDEFINED.
 */
int power_mgmt_get_saved_page(void);

/**
 * Crash info returned by power_mgmt_get_crash_info().
 */
typedef struct {
    uint32_t crash_count;          /**< Number of crash resets since last power-on */
    uint32_t last_crash_reason;    /**< esp_reset_reason_t of last crash (0 if none) */
    uint32_t boot_count;           /**< Total boot count (from NVS) */
} power_mgmt_crash_info_t;

/**
 * Check reset reason and update RTC crash counters.
 * Call early in app_main() after power_mgmt_check_wake_cause().
 * Increments crash count on PANIC / INT_WDT / TASK_WDT resets;
 * clears crash count on POWERON reset.
 */
void power_mgmt_check_crash(void);

/**
 * Get crash info (crash count, last reason, boot count).
 */
power_mgmt_crash_info_t power_mgmt_get_crash_info(void);
