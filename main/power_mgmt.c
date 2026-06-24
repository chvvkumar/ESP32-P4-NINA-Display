#include "power_mgmt.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "power_mgmt";

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

void power_mgmt_enter_deep_sleep(uint32_t wake_timer_s)
{
    /* Mark intent so wake-cause logging can distinguish intentional sleep.
     * The page is no longer saved — the arbiter resolves it fresh on wake. */
    s_deep_sleep_intended = true;

    if (wake_timer_s > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)wake_timer_s * 1000000ULL);
        ESP_LOGI(TAG, "Entering deep sleep with timer wakeup in %lu s",
                 (unsigned long)wake_timer_s);
    } else {
        ESP_LOGI(TAG, "Entering deep sleep with NO timer wakeup");
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
        ESP_LOGI(TAG, "Woke from deep sleep: cause=%d, intentional=%s",
                 (int)cause,
                 s_deep_sleep_intended ? "yes" : "no");
    }

    /* Clear the flag after reading so subsequent queries reflect reality */
    s_deep_sleep_intended = false;

    return cause;
}

void power_mgmt_check_crash(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    if (reason == ESP_RST_POWERON) {
        /* Fresh power-on clears crash history */
        s_crash_count = 0;
        s_last_crash_reason = 0;
        ESP_LOGI(TAG, "Power-on reset — crash history cleared");
    } else if (power_mgmt_reset_is_abnormal((uint32_t)reason)) {
        s_crash_count++;
        s_last_crash_reason = (uint32_t)reason;
        ESP_LOGW(TAG, "Crash reset detected: reason=%d (%s), crash_count=%lu",
                 (int)reason, power_mgmt_reset_reason_str((uint32_t)reason),
                 (unsigned long)s_crash_count);
    }
}

const char *power_mgmt_reset_reason_str(uint32_t reason)
{
    switch ((esp_reset_reason_t)reason) {
        case ESP_RST_UNKNOWN:    return "Unknown";
        case ESP_RST_POWERON:    return "Power-on";
        case ESP_RST_EXT:        return "External pin";
        case ESP_RST_SW:         return "Software restart";
        case ESP_RST_PANIC:      return "Panic / exception";
        case ESP_RST_INT_WDT:    return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:   return "Task watchdog";
        case ESP_RST_WDT:        return "Other watchdog";
        case ESP_RST_DEEPSLEEP:  return "Deep sleep wake";
        case ESP_RST_BROWNOUT:   return "Brownout / power loss";
        case ESP_RST_SDIO:       return "SDIO reset";
        case ESP_RST_USB:        return "USB peripheral reset";
        case ESP_RST_JTAG:       return "JTAG reset";
#if defined(ESP_RST_EFUSE)
        case ESP_RST_EFUSE:      return "eFuse error";
#endif
#if defined(ESP_RST_PWR_GLITCH)
        case ESP_RST_PWR_GLITCH: return "Power glitch";
#endif
#if defined(ESP_RST_CPU_LOCKUP)
        case ESP_RST_CPU_LOCKUP: return "CPU lockup";
#endif
        default:                 return "Unknown";
    }
}

bool power_mgmt_reset_is_abnormal(uint32_t reason)
{
    switch ((esp_reset_reason_t)reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
#if defined(ESP_RST_CPU_LOCKUP)
        case ESP_RST_CPU_LOCKUP:
#endif
            return true;
        default:
            return false;
    }
}

uint32_t power_mgmt_get_last_reset_reason(void)
{
    return (uint32_t)esp_reset_reason();
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
