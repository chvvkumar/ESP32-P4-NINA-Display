#include "goes_client.h"
#include "jpeg_utils.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "goes_client";

#define GOES_JPEG_MAX_SIZE   (512 * 1024)
#define GOES_HTTP_BUF_SIZE   4096
#define GOES_HTTP_TIMEOUT_MS 30000

void goes_data_init(goes_data_t *data)
{
    memset(data, 0, sizeof(*data));
    data->mutex = xSemaphoreCreateMutex();
}

bool goes_data_lock(goes_data_t *data, int timeout_ms)
{
    if (!data || !data->mutex) return false;
    return xSemaphoreTake(data->mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void goes_data_unlock(goes_data_t *data)
{
    if (data && data->mutex) xSemaphoreGive(data->mutex);
}

void goes_client_cleanup(goes_data_t *data)
{
    if (!data) return;
    if (goes_data_lock(data, 1000)) {
        if (data->image_buf) {
            heap_caps_free(data->image_buf);
            data->image_buf = NULL;
        }
        data->image_w = 0;
        data->image_h = 0;
        data->connected = false;
        goes_data_unlock(data);
    }
}

/* NESDIS sectors publish different size ladders. Return the GEOCOLOR image
 * size string (WxH, no extension) the device should fetch for a given sector.
 * Square regional sectors use 600x600 or 500x500; wide oceanic/continental
 * sectors are non-square. Unknown regions default to 600x600. */
static const char *goes_region_size(const char *region)
{
    static const struct { const char *code; const char *size; } sizes[] = {
        /* 600x600 square */
        {"umv","600x600"}, {"cgl","600x600"}, {"ne","600x600"}, {"se","600x600"},
        {"smv","600x600"}, {"sp","600x600"},  {"nr","600x600"}, {"sr","600x600"},
        {"pnw","600x600"}, {"psw","600x600"}, {"pr","600x600"},
        /* 500x500 square */
        {"eus","500x500"}, {"cam","500x500"}, {"car","500x500"},
        {"mex","500x500"}, {"ga","500x500"},
        /* 900x540 wide (5:3) */
        {"na","900x540"},  {"ssa","900x540"}, {"eep","900x540"},
        {"taw","900x540"}, {"nsa","900x540"},
        /* 560x280 wide (2:1) */
        {"can","560x280"},
    };
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        if (strcmp(sizes[i].code, region) == 0) return sizes[i].size;
    }
    return "600x600";
}

esp_err_t goes_client_poll(const char *region, goes_data_t *data)
{
    if (!region || !data) return ESP_ERR_INVALID_ARG;

    const char *size = goes_region_size(region);
    char url[192];
    snprintf(url, sizeof(url),
             "https://cdn.star.nesdis.noaa.gov/GOES19/ABI/SECTOR/%s/GEOCOLOR/%s.jpg",
             region, size);

    ESP_LOGI(TAG, "Fetching %s", url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = GOES_HTTP_TIMEOUT_MS,
        .buffer_size = GOES_HTTP_BUF_SIZE,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        if (goes_data_lock(data, 1000)) {
            data->connected = false;
            goes_data_unlock(data);
        }
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        if (goes_data_lock(data, 1000)) {
            data->connected = false;
            goes_data_unlock(data);
        }
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status %d", status);
        esp_http_client_cleanup(client);
        if (goes_data_lock(data, 1000)) {
            data->connected = false;
            goes_data_unlock(data);
        }
        return ESP_FAIL;
    }

    if (content_length <= 0) {
        content_length = GOES_JPEG_MAX_SIZE;
    }
    if (content_length > GOES_JPEG_MAX_SIZE) {
        content_length = GOES_JPEG_MAX_SIZE;
    }

    uint8_t *jpeg_buf = heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM);
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed for JPEG (%d bytes)", content_length);
        esp_http_client_cleanup(client);
        if (goes_data_lock(data, 1000)) {
            data->connected = false;
            goes_data_unlock(data);
        }
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    while (total_read < content_length) {
        int read_len = esp_http_client_read(client, (char *)(jpeg_buf + total_read),
                                            content_length - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }

    esp_http_client_cleanup(client);

    if (total_read < 1000) {
        ESP_LOGW(TAG, "JPEG too small (%d bytes), likely error page", total_read);
        heap_caps_free(jpeg_buf);
        if (goes_data_lock(data, 1000)) {
            data->connected = false;
            goes_data_unlock(data);
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloaded %d bytes JPEG, decoding...", total_read);

    uint8_t *rgb565 = NULL;
    uint32_t out_w = 0, out_h = 0;
    size_t out_size = 0;

    bool decoded = jpeg_sw_decode_rgb565(jpeg_buf, total_read,
                                          &rgb565, &out_w, &out_h, &out_size);
    heap_caps_free(jpeg_buf);

    if (!decoded || !rgb565) {
        ESP_LOGE(TAG, "JPEG decode failed");
        if (rgb565) heap_caps_free(rgb565);
        if (goes_data_lock(data, 1000)) {
            data->connected = false;
            goes_data_unlock(data);
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Decoded %lux%lu (%u bytes)", out_w, out_h, (unsigned)out_size);

    if (goes_data_lock(data, 2000)) {
        uint8_t *old = data->image_buf;
        data->image_buf = rgb565;
        data->image_w = (uint16_t)out_w;
        data->image_h = (uint16_t)out_h;
        data->connected = true;
        data->last_poll_ms = esp_timer_get_time() / 1000;
        goes_data_unlock(data);
        if (old) heap_caps_free(old);
    } else {
        heap_caps_free(rgb565);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
