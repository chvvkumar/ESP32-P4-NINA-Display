#pragma once
/* Host shim for esp_log.h — prints to stderr instead of the IDF log backend. */

#include <stdio.h>

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;

#define ESP_LOG_LEVEL(level, tag, format, ...) \
    fprintf(stderr, "[%d/%s] " format "\n", (int)(level), tag, ##__VA_ARGS__)

#define ESP_LOGE(tag, format, ...) fprintf(stderr, "E (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) fprintf(stderr, "W (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) fprintf(stderr, "I (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) fprintf(stderr, "D (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) fprintf(stderr, "V (%s) " format "\n", tag, ##__VA_ARGS__)

/* Buffer/hex dump helpers occasionally used by firmware code; no-ops on host. */
#define ESP_LOG_BUFFER_HEX(tag, buffer, buff_len) \
    do { (void)(tag); (void)(buffer); (void)(buff_len); } while (0)
#define ESP_LOG_BUFFER_HEXDUMP(tag, buffer, buff_len, level) \
    do { (void)(tag); (void)(buffer); (void)(buff_len); (void)(level); } while (0)

static inline void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    (void)tag;
    (void)level;
}
