#include "goes_client.h"
#include "jpeg_utils.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "goes_client";

#define GOES_JPEG_MAX_SIZE   (1024 * 1024)   /* 1MB: fits SOHO LASCO C3 1024 (~815KB) */
#define GOES_HTTP_BUF_SIZE   4096
#define GOES_HTTP_TIMEOUT_MS 30000
#define GOES_IMG_MAX_DIM     1024            /* reject images wider/taller than this before decode */
#define GOES_MAX_REDIRECTS   5               /* follow up to this many 30x Location hops */

/* Set a human-readable failure reason under the goes lock and mark disconnected.
 * Centralizes the lock boilerplate so every failure exit can record a reason. */
static void set_error_msg(goes_data_t *d, const char *m)
{
    if (!d) return;
    if (goes_data_lock(d, 1000)) {
        strlcpy(d->error_msg, m ? m : "", sizeof(d->error_msg));
        d->connected = false;
        goes_data_unlock(d);
    }
}

void goes_data_init(goes_data_t *data)
{
    memset(data, 0, sizeof(*data));
    data->src_kind = -1;
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
        data->src_kind = -1;
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

/* Solar imagery: SDO/AIA 1024px JPEGs (NASA SDO) plus SOHO realtime 1024px JPEGs
 * (SOHO/EIT, LASCO coronagraphs, SDO/HMI). All 1024px; LASCO C3 is ~815KB, so
 * GOES_JPEG_MAX_SIZE is 1MB to fit them. Index 0..17. */
#define SOLAR_BAND_COUNT 18
static const char *SOLAR_URLS[SOLAR_BAND_COUNT] = {
    "https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_0094.jpg",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_0131.jpg",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_0171.jpg",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_0193.jpg",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_0211.jpg",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_0304.jpg",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_0335.jpg",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_1600.jpg",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_1700.jpg",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_4500.jpg",
    "https://soho.nascom.nasa.gov/data/realtime/c2/1024/latest.jpg",
    "https://soho.nascom.nasa.gov/data/realtime/c3/1024/latest.jpg",
    "https://soho.nascom.nasa.gov/data/realtime/eit_171/1024/latest.jpg",
    "https://soho.nascom.nasa.gov/data/realtime/eit_195/1024/latest.jpg",
    "https://soho.nascom.nasa.gov/data/realtime/eit_284/1024/latest.jpg",
    "https://soho.nascom.nasa.gov/data/realtime/eit_304/1024/latest.jpg",
    "https://soho.nascom.nasa.gov/data/realtime/hmi_igr/1024/latest.jpg",
    "https://soho.nascom.nasa.gov/data/realtime/hmi_mag/1024/latest.jpg",
};
static const char *SOLAR_LABELS[SOLAR_BAND_COUNT] = {
    "AIA 94","AIA 131","AIA 171","AIA 193","AIA 211","AIA 304","AIA 335","AIA 1600","AIA 1700","AIA 4500",
    "LASCO C2","LASCO C3","SOHO EIT 171","SOHO EIT 195","SOHO EIT 284","SOHO EIT 304",
    "HMI Continuum","HMI Magnetogram"
};

const char *solar_band_url(uint8_t idx)   { return idx < SOLAR_BAND_COUNT ? SOLAR_URLS[idx]   : SOLAR_URLS[0]; }
const char *solar_band_label(uint8_t idx) { return idx < SOLAR_BAND_COUNT ? SOLAR_LABELS[idx] : SOLAR_LABELS[0]; }

/* Solar source images (SDO/AIA via sdo.gsfc.nasa.gov, SOHO/HMI via
 * soho.nascom.nasa.gov) are delivered upright (north up, caption at bottom), so
 * no vertical flip is applied. Any build-environment orientation difference is
 * corrected by the per-source `solar_orientation` rotation setting, not here.
 * (A vertical flip is a mirror and would put the caption at the top and mirror
 * sunspot positions, which no rotation setting can undo.) */
bool solar_band_vflip(uint8_t idx) { (void)idx; return false; }

/* Per-band center-crop percentage (100 = no crop). AIA (0..9) and SOHO EIT
 * (12..15) have a wide source border, so 88% zooms past the timestamp/label.
 * HMI continuum/magnetogram (16,17) have only a ~4.2% black margin around a
 * ~91.5%-diameter disc, so 92% trims the caption border while leaving a thin
 * black ring (disc NOT clipped). LASCO C2/C3 (10,11) fill the frame edge-to-edge
 * with a burned-in timestamp at the very bottom (~3.5% up); 90% (~5%/side) crops
 * it off, sacrificing some outer corona (acceptable) and avoiding a blend patch. */
uint8_t solar_band_crop_pct(uint8_t idx)
{
    if (idx == 10 || idx == 11) return 90;   /* LASCO: crop off the burned-in timestamp */
    if (idx == 16 || idx == 17) return 92;   /* HMI: trim to disc edge */
    return 88;                               /* AIA / SOHO EIT */
}

/* Upright-full-image fractional rect (0..1, origin top-left, y down) covering a
 * caption that the crop alone does not remove. Returns false when no mask is
 * needed. The HMI rect is below the disc bottom (~row 0.97) and normally cropped
 * away (belt-and-suspenders for a taller-than-expected future caption). LASCO is
 * cropped instead of masked, so it returns false here. */
bool solar_band_text_mask(uint8_t idx, float *x0, float *y0, float *x1, float *y1)
{
    float a, b, c, d;
    switch (idx) {
        case 16:
        case 17: a = 0.0f; b = 0.97f; c = 0.50f; d = 1.0f; break;  /* HMI */
        default: return false;  /* LASCO (10,11) now cropped, not masked */
    }
    if (x0) *x0 = a;
    if (y0) *y0 = b;
    if (x1) *x1 = c;
    if (y1) *y1 = d;
    return true;
}

/* NESDIS sector code -> human-readable region name. Single source of truth
 * (moved out of nina_image_display.c). Returns the code itself if no match. */
const char *goes_region_name(const char *code)
{
    static const struct { const char *code; const char *name; } region_labels[] = {
        {"umv","Upper Mississippi Valley"}, {"cgl","Great Lakes"},
        {"ne","Northeast"},                 {"se","Southeast"},
        {"smv","Southern Mississippi Valley"},{"sp","Southern Plains"},
        {"nr","Northern Rockies"},          {"sr","Southern Rockies"},
        {"pnw","Pacific Northwest"},        {"psw","Pacific Southwest"},
        {"pr","Puerto Rico"},               {"eus","U.S. Atlantic Coast"},
        {"cam","Central America"},          {"car","Caribbean"},
        {"mex","Mexico"},                   {"ga","Gulf of America"},
        {"na","Northern Atlantic"},         {"ssa","South America (South)"},
        {"eep","Eastern Pacific"},          {"taw","Tropical Atlantic"},
        {"nsa","South America (North)"},    {"can","Canada"},
        {NULL,NULL}
    };
    if (!code) return "";
    for (int i = 0; region_labels[i].code; i++) {
        if (!strcmp(region_labels[i].code, code)) return region_labels[i].name;
    }
    return code;
}

esp_err_t goes_client_poll(const char *region, goes_data_t *data)
{
    if (!region || !data) return ESP_ERR_INVALID_ARG;

    const char *size = goes_region_size(region);
    char url[192];
    snprintf(url, sizeof(url),
             "https://cdn.star.nesdis.noaa.gov/GOES19/ABI/SECTOR/%s/GEOCOLOR/%s.jpg",
             region, size);

    /* The decode+render pipeline on this panel applies a net vertical flip, so the
     * NESDIS GOES JPEG must be flipped (vflip=true) to display north-up. This matches
     * the Solar path, which also uses vflip=true. Verified on-device: vflip=false
     * renders the image upside-down (issue #166 regression). */
    return goes_client_poll_url(url, data, true, goes_region_name(region), 0 /* GOES */);
}

esp_err_t goes_client_poll_url(const char *url, goes_data_t *data, bool vflip, const char *label, int8_t src_kind)
{
    if (!url || !data) return ESP_ERR_INVALID_ARG;

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
        set_error_msg(data, "Fetch failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        set_error_msg(data, "Fetch failed");
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    /* Follow Location redirects manually: the streaming open()/fetch_headers()
     * path does NOT auto-follow. For each 30x, esp_http_client_set_redirection()
     * adopts the captured Location URL; we then re-open + re-fetch_headers on the
     * new URL. Cap the chain to avoid loops. crt_bundle already covers TLS hosts. */
    int redirects = 0;
    while ((status == 301 || status == 302 || status == 307 || status == 308) &&
           redirects < GOES_MAX_REDIRECTS) {
        ESP_LOGI(TAG, "HTTP %d redirect, following (hop %d)", status, redirects + 1);
        err = esp_http_client_set_redirection(client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set_redirection failed: %s", esp_err_to_name(err));
            break;
        }
        /* Close the prior response's socket before reconnecting to the redirect
         * target. Without this, the previous connection leaks for the duration of
         * the chain. cleanup() on the terminal path still closes exactly once. */
        esp_http_client_close(client);
        /* Re-issue the request against the new (redirected) URL. */
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP re-open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            set_error_msg(data, "Fetch failed");
            return err;
        }
        content_length = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        redirects++;
    }

    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status %d", status);
        esp_http_client_cleanup(client);
        set_error_msg(data, "Fetch failed");
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
        set_error_msg(data, "Out of memory");
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
        set_error_msg(data, "Fetch failed");
        return ESP_FAIL;
    }

    /* JPEG magic gate: SOI marker is FF D8. Reject non-JPEG payloads (HTML error
     * pages, PNG, etc.) before handing anything to the decoder. */
    if (jpeg_buf[0] != 0xFF || jpeg_buf[1] != 0xD8) {
        ESP_LOGW(TAG, "Not a JPEG (magic %02X %02X)", jpeg_buf[0], jpeg_buf[1]);
        heap_caps_free(jpeg_buf);
        set_error_msg(data, "Not a JPEG image");
        return ESP_FAIL;
    }

    /* Pre-decode dimension cap: a small JPEG can decode to enormous dimensions,
     * so probe width/height from the header and reject BEFORE the big decode
     * allocation to avoid OOM. */
    uint32_t probe_w = 0, probe_h = 0;
    if (jpeg_probe_dimensions(jpeg_buf, total_read, &probe_w, &probe_h)) {
        if (probe_w > GOES_IMG_MAX_DIM || probe_h > GOES_IMG_MAX_DIM) {
            ESP_LOGW(TAG, "JPEG too large: %lux%lu (max %d)",
                     (unsigned long)probe_w, (unsigned long)probe_h, GOES_IMG_MAX_DIM);
            heap_caps_free(jpeg_buf);
            set_error_msg(data, "Image too large (max 1024px)");
            return ESP_FAIL;
        }
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
        set_error_msg(data, "Decode failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Decoded %lux%lu (%u bytes)", out_w, out_h, (unsigned)out_size);

    if (goes_data_lock(data, 2000)) {
        uint8_t *old = data->image_buf;
        data->image_buf = rgb565;
        data->image_w = (uint16_t)out_w;
        data->image_h = (uint16_t)out_h;
        data->vflip = vflip;
        if (label) { strlcpy(data->label, label, sizeof(data->label)); }
        else       { data->label[0] = '\0'; }
        data->src_kind = src_kind;
        data->error_msg[0] = '\0';   /* clear: this fetch succeeded */
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
