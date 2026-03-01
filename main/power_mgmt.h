#pragma once

#include "esp_err.h"
#include "esp_sleep.h"

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
