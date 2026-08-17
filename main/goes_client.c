#include "goes_client.h"
#include "jpeg_utils.h"
#include "http_fetch.h"
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
 * no vertical flip is applied. (Historical "build-environment orientation
 * differences" were stb's flip-on-load flag reading uninitialized TLS garbage;
 * see STBI_NO_THREAD_LOCALS in stb_image.c, fixed 2026-08-14.) */
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

/* Shared fetch + format/dimension probe + software decode. Handles every format
 * stb_image is compiled for (JPEG, PNG, GIF - the last one for NWS RIDGE-2
 * radar tiles). Writes the decoded frame into *out (top-down rows; see
 * STBI_NO_THREAD_LOCALS in stb_image.c for why no flip is applied anywhere any
 * more).
 *
 * COMPRESSED-BYTE RETENTION (@p out_src / @p out_src_len, both NULL or both
 * non-NULL). NULL is the normal case and behaves exactly as before: the
 * compressed buffer is freed here the moment the decode is done. Non-NULL asks
 * for the source bytes to SURVIVE the call, and then, on ESP_OK only,
 * *out_src is a PSRAM allocation the CALLER must heap_caps_free(). On every
 * failure path (and on a decode failure specifically) the buffer is freed here
 * and *out_src stays NULL, so the caller frees only what it was handed.
 *
 * The radar ring is the one caller that asks: it re-derives its frames locally
 * on a crop / dark-mode / Red Night change instead of re-downloading ~370 KB. */
static esp_err_t fetch_image_into(const char *url, const char *label, image_frame_t *out,
                                  uint8_t **out_src, size_t *out_src_len)
{
    if (!url || !out) return ESP_ERR_INVALID_ARG;
    if (out_src) { *out_src = NULL; *out_src_len = 0; }

    ESP_LOGI(TAG, "Fetching %s", url);

    /* Shared binary fetch shell (client setup, manual redirect chain, sizing,
     * read loop). CLAMP: an oversized Content-Length is truncated to the cap
     * rather than rejected, so a too-large tile still gets the format checks
     * below rather than failing outright. The User-Agent rides in on
     * extra_header (the binary opts have no dedicated user_agent field, and
     * apply_headers() replaces esp_http_client's default): NWS asks automated
     * clients to identify themselves and Iowa Environmental Mesonet blocks
     * anonymous ones outright. */
    const http_fetch_binary_opts_t bopts = {
        .timeout_ms = GOES_HTTP_TIMEOUT_MS,
        .use_tls_bundle = true,
        .max_redirects = GOES_MAX_REDIRECTS,
        .rx_buffer_size = GOES_HTTP_BUF_SIZE,
        .tx_buffer_size = 1024,
        .max_size = GOES_JPEG_MAX_SIZE,
        .oversize = HTTP_BIN_OVERSIZE_CLAMP,
        .extra_header = "User-Agent: NINA-Display/1.0 (ESP32-P4 dashboard)",
        .label = "image",
    };

    uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;
    esp_err_t err = http_fetch_binary(url, &bopts, &jpeg_buf, &jpeg_len);
    if (err != ESP_OK) {
        strlcpy(out->error_msg, err == ESP_ERR_NO_MEM ? "Out of memory" : "Fetch failed", sizeof(out->error_msg));
        return err;
    }
    int total_read = (int)jpeg_len;

    if (total_read < 1000) {
        ESP_LOGW(TAG, "Image too small (%d bytes), likely error page", total_read);
        heap_caps_free(jpeg_buf);
        strlcpy(out->error_msg, "Fetch failed", sizeof(out->error_msg));
        return ESP_FAIL;
    }

    /* Format gate + pre-decode dimension cap in one pass: reject payloads the
     * decoder cannot handle (HTML error pages) before touching it, and reject
     * oversized images BEFORE the big decode allocation to avoid OOM -- a small
     * compressed file can expand to enormous dimensions. Dimensions of 0 mean
     * the header was too short to read them, so the cap is skipped. */
    uint32_t probe_w = 0, probe_h = 0;
    img_fmt_t fmt = image_probe_format_dims(jpeg_buf, (size_t)total_read, &probe_w, &probe_h);
    if (fmt == IMG_FMT_UNKNOWN) {
        ESP_LOGW(TAG, "Unsupported image format (magic %02X %02X)", jpeg_buf[0], jpeg_buf[1]);
        heap_caps_free(jpeg_buf);
        strlcpy(out->error_msg, "Unsupported image format", sizeof(out->error_msg));
        return ESP_FAIL;
    }
    if ((probe_w > GOES_IMG_MAX_DIM) || (probe_h > GOES_IMG_MAX_DIM)) {
        ESP_LOGW(TAG, "Image too large: %lux%lu (max %d)",
                 (unsigned long)probe_w, (unsigned long)probe_h, GOES_IMG_MAX_DIM);
        heap_caps_free(jpeg_buf);
        strlcpy(out->error_msg, "Image too large (max 1024px)", sizeof(out->error_msg));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloaded %d bytes (fmt %d), decoding...", total_read, (int)fmt);

    uint8_t *rgb565 = NULL;
    uint32_t out_w = 0, out_h = 0;
    size_t out_size = 0;
    bool decoded = jpeg_sw_decode_rgb565(jpeg_buf, total_read, &rgb565, &out_w, &out_h, &out_size);
    if (out_src && decoded && rgb565) {
        *out_src     = jpeg_buf;              /* ownership moves to the caller */
        *out_src_len = (size_t)total_read;
    } else {
        heap_caps_free(jpeg_buf);
    }
    if (!decoded || !rgb565) {
        ESP_LOGE(TAG, "Image decode failed");
        if (rgb565) heap_caps_free(rgb565);
        strlcpy(out->error_msg, "Decode failed", sizeof(out->error_msg));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Decoded %lux%lu (%u bytes)", out_w, out_h, (unsigned)out_size);

    out->buf = rgb565;
    out->w = (uint16_t)out_w;
    out->h = (uint16_t)out_h;
    strlcpy(out->label, label ? label : "", sizeof(out->label));
    out->error_msg[0] = '\0';
    out->stamp_ms = esp_timer_get_time() / 1000;
    return ESP_OK;
}

esp_err_t image_fetch_goes(const char *region, image_frame_t *out)
{
    if (!region || !region[0] || !out) return ESP_ERR_INVALID_ARG;
    const char *size = goes_region_size(region);
    char url[192];
    snprintf(url, sizeof(url),
             "https://cdn.star.nesdis.noaa.gov/GOES19/ABI/SECTOR/%s/GEOCOLOR/%s.jpg",
             region, size);
    return fetch_image_into(url, goes_region_name(region), out, NULL, NULL);
}

esp_err_t image_fetch_solar(uint8_t band, image_frame_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    return fetch_image_into(solar_band_url(band), solar_band_label(band), out, NULL, NULL);
}

esp_err_t image_fetch_custom(const char *url, image_frame_t *out)
{
    if (!url || !url[0] || !out) return ESP_ERR_INVALID_ARG;
    return fetch_image_into(url, "Custom", out, NULL, NULL);
}

esp_err_t image_fetch_custom_retain(const char *url, image_frame_t *out,
                                    uint8_t **out_src, size_t *out_src_len)
{
    if (!out_src || !out_src_len) return ESP_ERR_INVALID_ARG;
    *out_src = NULL;
    *out_src_len = 0;
    if (!url || !url[0] || !out) return ESP_ERR_INVALID_ARG;
    return fetch_image_into(url, "Custom", out, out_src, out_src_len);
}
