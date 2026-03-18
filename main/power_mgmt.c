#include "power_mgmt.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "power_mgmt";

RTC_DATA_ATTR static int  s_saved_page          = 0;
RTC_DATA_ATTR static bool s_deep_sleep_intended  = false;
RTC_DATA_ATTR static uint32_t s_crash_count        = 0;
RTC_DATA_ATTR static uint32_t s_last_crash_reason  = 0;  /* esp_reset_reason_t */

esp_err_t power_mgmt_init(void)
{
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 360,
        .min_freq_mhz = 40,
        .light_sleep_enable = false,
    };

    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "DFS enabled: max=%d MHz, min=%d MHz, light_sleep=%s",
                 pm_config.max_freq_mhz,
                 pm_config.min_freq_mhz,
                 pm_config.light_sleep_enable ? "on" : "off");
    } else {
        ESP_LOGE(TAG, "Failed to configure power management: %s", esp_err_to_name(err));
    }

    return err;
}

void power_mgmt_enter_deep_sleep(uint32_t wake_timer_s, int current_page)
{
    /* Persist state to RTC memory so we can restore after wake */
    s_saved_page         = current_page;
    s_deep_sleep_intended = true;

    if (wake_timer_s > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)wake_timer_s * 1000000ULL);
        ESP_LOGI(TAG, "Entering deep sleep with timer wakeup in %lu s (page %d saved)",
                 (unsigned long)wake_timer_s, current_page);
    } else {
        ESP_LOGI(TAG, "Entering deep sleep with NO timer wakeup (page %d saved)", current_page);
    }

    /* Cleanly shut down WiFi before sleeping */
    esp_wifi_stop();

    /* This call does NOT return — the chip resets on wake */
    esp_deep_sleep_start();
}

esp_sleep_wakeup_cause_t power_mgmt_check_wake_cause(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "Woke from deep sleep: cause=%d, intentional=%s, saved_page=%d",
                 (int)cause,
                 s_deep_sleep_intended ? "yes" : "no",
                 s_saved_page);
    }

    /* Clear the flag after reading so subsequent queries reflect reality */
    s_deep_sleep_intended = false;

    return cause;
}

int power_mgmt_get_saved_page(void)
{
    return s_saved_page;
}

void power_mgmt_check_crash(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    if (reason == ESP_RST_POWERON) {
        /* Fresh power-on clears crash history */
        s_crash_count = 0;
        s_last_crash_reason = 0;
        ESP_LOGI(TAG, "Power-on reset — crash history cleared");
    } else if (reason == ESP_RST_PANIC ||
               reason == ESP_RST_INT_WDT ||
               reason == ESP_RST_TASK_WDT) {
        s_crash_count++;
        s_last_crash_reason = (uint32_t)reason;
        ESP_LOGW(TAG, "Crash reset detected: reason=%d, crash_count=%lu",
                 (int)reason, (unsigned long)s_crash_count);
    }
}

power_mgmt_crash_info_t power_mgmt_get_crash_info(void)
{
    uint32_t boot_count = 0;
    nvs_handle_t nvs;
    if (nvs_open("system", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "boot_cnt", &boot_count);
        nvs_close(nvs);
    }

    return (power_mgmt_crash_info_t){
        .crash_count       = s_crash_count,
        .last_crash_reason = s_last_crash_reason,
        .boot_count        = boot_count,
    };
}
