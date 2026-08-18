/**
 * @file image_page_poll.c
 * @brief Pollers for the five image pages, on the poll_task spine. One task
 *        per ENABLED source ("img_goes" .. "img_radar", 12288 B PSRAM stack,
 *        prio 3, Core 0), gated by image_page_t.poll_gate (active || warm).
 *        GOES/Solar/Custom/Radar share net_poll_once(); the Moon renders locally in
 *        moon_poll_once() (ported from the former goes_poll_task moon branch)
 *        and releases its texture/scratch in moon_on_park().
 */

#include "ui/nina_image_page.h"
#include "ui/nina_wait_overlay.h"
#include "ui/nina_toast.h"
#include "poll_task.h"
#include "radar_play.h"            /* radar_frame_is_stale (ring generation) */
#include "radar_wms.h"             /* styles 1/2: GeoServer WMS URL builder + caps TIME parser */
#include "radar_sites.h"           /* radar_site_coords (WMS site bbox centre) */
#include "http_fetch.h"            /* http_fetch_text (WMS GetCapabilities) */
#include "tasks.h"                 /* s_wifi_event_group, WIFI_CONNECTED_BIT, ota_in_progress */
#include "app_config.h"
#include "display_defs.h"
#include "moon_sphere.h"
#include "moon_ephemeris.h"
#include "moon_interaction.h"
#include "jpeg_utils.h"            /* ppa_scale_rgb565_into_noclear */
#include "image_red_remap.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"             /* esp_cache_msync - flush the PPA-source guard rows once */
#include "esp_timer.h"
#include <math.h>                  /* expf, fmod, lround */
#include <stdio.h>                 /* snprintf */
#include <string.h>
#include <time.h>

static const char *TAG = "image_poll";

/* Moon state, drag scratch, caption, rise/set cache, overlay info.
 * Moved verbatim from main/tasks.c (the block that ran on goes_poll_task),
 * minus the retired image_display_manual_fetch flag (per-instance now) and the
 * s_moon_release_req ownership hand-off (replaced by moon_on_park() below). */

/* Last-computed moon state, written only by goes_poll_task (single writer) and
 * read by moon_caption() from the UI task. The fields are slow-changing and the
 * read is benign (pointer + float), so a lock is intentionally omitted. */
static moon_state_t s_moon_state;

/* One-shot tap-animation request. Set by the moon-page tap handler (UI/Core 1),
 * consumed once by goes_poll_task via atomic_exchange. Declared extern in tasks.h. */
_Atomic bool moon_anim_request = false;

/* ── Moon drag-to-rotate scratch + PPA output buffers ────────────────────────
 * Persistent PSRAM buffers reused across every drag frame so the realtime loop
 * does ZERO per-frame heap alloc. Lazily allocated on the first drag of a moon-
 * page visit and freed on page leave via moon_drag_buffers_free(). Owned solely
 * by goes_poll_task.
 *
 * Touch-frame pipeline (per frame):
 *   1. moon_sphere_render_into() the sphere at MOON_DRAG_SZ_TOUCH (240) into the
 *      small color/z scratch (s_drag_color / s_drag_zbuf), CPU-written;
 *   2. ppa_scale_rgb565_into_noclear() hardware-upscales 240->720 (an EXACT 3.0x
 *      ratio: 720/240=3, representable on PPA's 1/16 scale grid so the whole 720
 *      output is filled with no edge-streak remainder, and no per-frame memset)
 *      into the ping-pong output buffer s_ppa_out[ping];
 *   3. image_page_show_borrowed(p, s_ppa_out[ping], 720, 720) points the
 *      LVGL descriptor straight at it (no copy) at scale 1.0; then flip ping.
 * The PPA driver handles cache coherency for the BLOCKING transfer in BOTH
 * directions, so the output is coherent for LVGL read when the call returns — no
 * manual esp_cache_msync. The output is DOUBLE-BUFFERED (s_ppa_out[0]/[1]); PPA
 * writes the buffer NOT currently on screen so it never tears with the LVGL flush.
 *
 * 240->720 is the SINGLE touch render size for the whole interaction (active drag
 * AND the post-release settle ease). 240 was chosen because 720/240 = 3.0x is an
 * exact integer ratio on PPA (11520 % 240 == 0); 300 (the prior settle size) maps
 * to 2.40x which PPA truncates to 2.375x, leaving a ~8px unfilled streak.
 *
 * GUARD ROWS (required): SRM bilinear UPSCALE over-reads the source bottom edge.
 * At 3.0x the last output row (719) maps to source y≈239.67, so the sampler reads
 * source row 240 — one past the 240 valid rows. The driver does NOT clamp or
 * bounds-check the INPUT sampling, so without padding that read lands in adjacent
 * heap and renders as a colored garbage band along the bottom of the disc. This is
 * INDEPENDENT of the exact-ratio fix: 3.0x kills the fill STREAK (Gotcha 4), the
 * guard rows kill this OVER-READ. Allocating MOON_DRAG_GUARD_ROWS extra zeroed rows
 * below the valid region makes that boundary sample read our own black memory. The
 * rows are zeroed + flushed to PSRAM (C2M) ONCE at alloc; the per-frame render
 * writes only rows 0..239, so they stay zeroed (no per-frame memset/msync). PPA
 * in.pic_h/block_h stay 240 (the buffer is just taller). The right edge needs no
 * guard (col 240 wraps to col 0 of the next row, in-bounds; the last row's wrap
 * lands in the guard). This matches existing project practice — the GOES/decode PPA
 * paths in this file already memset their PPA source ("Zero buffer so PPA edge
 * interpolation reads black, not heap garbage"). See reference_lvgl_ppa_scale_limitation.md
 * (Gotcha 3/4): a clean PPA upscale needs BOTH the exact n/16 ratio AND guard rows.
 *
 *   - s_drag_color: (MOON_DRAG_SZ_TOUCH + GUARD_ROWS) * MOON_DRAG_SZ_TOUCH * 2  (CPU-written + guard)
 *   - s_drag_zbuf:  MOON_DRAG_SZ_TOUCH^2 * 2  depth buffer (not DMA-read by PPA; no guard needed)
 *   - s_ppa_out[2]: 720*720*2 each            PPA upscale output, ping-pong
 * If PPA scale fails or a buffer didn't allocate, the loop falls back to the
 * software-scale path (moon_sphere_render + image_page_show_scaled) so it
 * degrades gracefully instead of crashing. */
#define MOON_DRAG_SZ_TOUCH  240          /* render size for the whole touch interaction (240->720 = exact 3.0x) */
#define MOON_DRAG_GUARD_ROWS 8           /* zeroed rows below s_drag_color: catch PPA upscale bottom over-read */
#define MOON_DRAG_SECTORS   96           /* tessellation: longitude bands (kills quad faceting) */
#define MOON_DRAG_STACKS    48           /* tessellation: latitude bands */
/* Active-drag easing is per-frame exponential (responsive to the finger). The
 * release settle uses a TIME-based ease (see below) so it reads smooth regardless
 * of the achieved framerate (~14-19fps) instead of finishing in 2-3 jumpy frames. */
#define MOON_DRAG_EASE_A    0.35f        /* per-frame easing alpha while dragging */
#define MOON_DRAG_SETTLE_MS 450          /* target duration of the release ease-back */
#define MOON_DRAG_FRAME_US  22000        /* target frame period (cap; render usually dominates) */
#define MOON_DRAG_REST_GRACE_MS 250      /* idle hold after settle before committing the crisp resting render; a new touch within this window re-enters the drag instead (avoids the eager reset / blocking-render stall on rapid re-swipe) */
#define MOON_LIGHT_FADE_MS  300          /* explore lighting crossfade: 0->1 on touch-down, 1->0 once the disc is home */
/* Explore lighting mix (0 = true phase, 1 = fully lit) carried across drag
 * frames, outer-loop iterations and free-spin holds so the fade never restarts
 * mid-interaction. Written only by goes_poll_task; reset on page leave. */
static float     s_moon_light_mix = 0.0f;
static uint16_t *s_drag_color  = NULL;
static uint16_t *s_drag_zbuf   = NULL;
static uint16_t *s_ppa_out[2]  = { NULL, NULL };   /* 720x720 PPA-output ping-pong */
static int       s_ppa_ping    = 0;                /* index PPA writes next (flips each frame) */

/* Free the moon-drag scratch and PPA output buffers and NULL them. Called ONLY
 * from the Moon poll task at its parked point (moon_on_park() below), where no
 * render can be in flight -- the buffers are read/written by renders that run
 * only on that task, so no other task may free them. Safe to call when the
 * buffers were never allocated (NULL frees are no-ops). The page's own
 * software-scale copy buffers are freed separately in image_page_release_lvgl(). */
static void moon_drag_buffers_free(void)
{
    if (s_drag_color) { heap_caps_free(s_drag_color); s_drag_color = NULL; }
    if (s_drag_zbuf)  { heap_caps_free(s_drag_zbuf);  s_drag_zbuf  = NULL; }
    if (s_ppa_out[0]) { heap_caps_free(s_ppa_out[0]); s_ppa_out[0] = NULL; }
    if (s_ppa_out[1]) { heap_caps_free(s_ppa_out[1]); s_ppa_out[1] = NULL; }
    s_ppa_ping = 0;
}

/* Provide the caption text for the Image Display page when the Moon source is
 * active. Declared in ui/nina_image_page.h. */
void moon_caption(char *name_out, size_t name_sz, char *pct_out, size_t pct_sz)
{
    /* Lock-free read is acceptable here (slow-changing, single writer). */
    snprintf(name_out, name_sz, "%s", s_moon_state.phase_name ? s_moon_state.phase_name : "Moon");
    snprintf(pct_out, pct_sz, "%d%%", (int)(s_moon_state.illum * 100.0f + 0.5f));
}

/* Cached rise/set results — recomputed at most once per 30 s or on location
 * change. moon_rise_set() is a 457-sample double-precision scan (~22k soft-float
 * fmod on this FPU), so it runs ONLY on goes_poll_task (Core 0, no display lock);
 * moon_overlay_info() on the UI path (Core 1, display lock held) reads the cache.
 * The spinlock keeps the six fields a consistent set across the two cores. */
static portMUX_TYPE s_rs_mux = portMUX_INITIALIZER_UNLOCKED;
static time_t s_rs_calc_at  = 0;
static time_t s_rs_rise     = 0;
static time_t s_rs_set      = 0;
static bool   s_rs_rise_v   = false;
static bool   s_rs_set_v    = false;
static double s_rs_lat      = 1e9;
static double s_rs_lon      = 1e9;

/* Refresh the rise/set cache when stale (>= 30 s) or the location changed.
 * Call from goes_poll_task only — never with the display lock held. */
static void moon_rise_set_refresh(void)
{
    const app_config_t *cfg = app_config_get();
    double lat = cfg->moon_lat, lon = cfg->moon_lon;
    if (lat == 0.0 && lon == 0.0) { lat = cfg->weather_lat; lon = cfg->weather_lon; }

    time_t now;
    time(&now);

    portENTER_CRITICAL(&s_rs_mux);
    bool stale = (s_rs_calc_at == 0 || (now - s_rs_calc_at) >= 30 ||
                  lat != s_rs_lat || lon != s_rs_lon);
    portEXIT_CRITICAL(&s_rs_mux);
    if (!stale) return;

    time_t rise_t = 0, set_t = 0;
    bool   rise_v = false, set_v = false;
    moon_rise_set(now, lat, lon, &rise_t, &rise_v, &set_t, &set_v);

    portENTER_CRITICAL(&s_rs_mux);
    s_rs_rise    = rise_t;
    s_rs_rise_v  = rise_v;
    s_rs_set     = set_t;
    s_rs_set_v   = set_v;
    s_rs_lat     = lat;
    s_rs_lon     = lon;
    s_rs_calc_at = now;
    portEXIT_CRITICAL(&s_rs_mux);
}

/* Return the signed difference in LOCAL calendar days between evt and now.
 * Normalises both instants to local noon before subtracting so DST transitions
 * within the interval do not produce off-by-one errors. */
static int local_day_delta(time_t evt, time_t ref)
{
    struct tm ta, tb;
    localtime_r(&evt, &ta);
    localtime_r(&ref, &tb);
    ta.tm_hour = 12; ta.tm_min = 0; ta.tm_sec = 0; ta.tm_isdst = -1;
    tb.tm_hour = 12; tb.tm_min = 0; tb.tm_sec = 0; tb.tm_isdst = -1;
    time_t na = mktime(&ta);
    time_t nb = mktime(&tb);
    return (int)lround((double)(na - nb) / 86400.0);
}

/* Format a single moon event (rise or set) into `out[sz]`.
 * Output examples (all fit within 20 bytes including NUL):
 *   "Rise 14:32"       24h, same day
 *   "Rise 2:34am +1"   12h, next day
 *   "Set 09:02 -1"     24h, previous day
 *   "Rise --:--"       invalid */
static void fmt_moon_event(char *out, size_t sz, const char *label,
                           time_t evt, bool valid, time_t now_t, bool use_24h)
{
    if (!valid) {
        snprintf(out, sz, "%s --:--", label);
        return;
    }

    struct tm lt;
    localtime_r(&evt, &lt);

    /* Time string: 5 chars for 24h "HH:MM", up to 7 for 12h "12:34pm". */
    char tbuf[10];
    if (use_24h) {
        strftime(tbuf, sizeof(tbuf), "%H:%M", &lt);
    } else {
        int hr = lt.tm_hour % 12;
        if (hr == 0) hr = 12;
        snprintf(tbuf, sizeof(tbuf), "%d:%02d%s",
                 hr, lt.tm_min, lt.tm_hour < 12 ? "am" : "pm");
    }

    /* Day-offset suffix: "" / " +N" / " -N". Sized for the full int range so
     * -Werror=format-truncation is satisfied (delta is realistically +/-1..2). */
    char sfx[16] = "";
    int  delta   = local_day_delta(evt, now_t);
    if (delta > 0) {
        snprintf(sfx, sizeof(sfx), " +%d", delta);
    } else if (delta < 0) {
        snprintf(sfx, sizeof(sfx), " %d", delta);   /* "-N" via %d with negative */
    }

    /* Longest possible result: "Rise 12:34pm +1\0" = 16 chars — fits in 20. */
    snprintf(out, sz, "%s %s%s", label, tbuf, sfx);
}

/* Fills the four moon-overlay corner strings.  All buffers must be non-NULL.
 *   age:  "Age 11.2d"
 *   next: "Full in 3d" or "New in 18d" (whichever lunar event is sooner);
 *         "Full <1d" / "New <1d" when less than one day away.
 *   rise: "Rise 14:32"  or  "Rise --:--" (24h when cfg->weather_time_format==1,
 *         12h e.g. "Rise 2:34am" otherwise).  A day-offset suffix " +N" / " -N"
 *         is appended when the event falls on a different local calendar day.
 *   set:  same format as rise.
 * Reads cached s_moon_state (lock-free, single writer) for age/next; the rise/set
 * times come from the cache filled by moon_rise_set_refresh() on the poll task —
 * this function never runs the ephemeris scan, it is called under the display lock. */
void moon_overlay_info(char *age,  size_t age_sz,
                       char *next, size_t next_sz,
                       char *rise, size_t rise_sz,
                       char *set,  size_t set_sz)
{
    const double SYNODIC = 29.530588853;

    /* --- Age --- */
    double cycle = (double)s_moon_state.cycle;
    double age_days = cycle * SYNODIC;
    snprintf(age, age_sz, "Age %.1fd", age_days);

    /* --- Next phase (Full or New, whichever is sooner) --- */
    double days_to_full = fmod(0.5 - cycle + 1.0, 1.0) * SYNODIC;
    double days_to_new  = fmod(1.0 - cycle,        1.0) * SYNODIC;
    /* fmod(1.0 - 0.0, 1.0) == 0.0, which means "already new"; push to a full
     * cycle so we report "New in 29.5d" rather than "New in 0d". */
    if (days_to_new < 0.01) days_to_new = SYNODIC;

    if (days_to_full <= days_to_new) {
        if (days_to_full < 1.0)
            snprintf(next, next_sz, "Full <1d");
        else
            snprintf(next, next_sz, "Full in %.0fd", days_to_full);
    } else {
        if (days_to_new < 1.0)
            snprintf(next, next_sz, "New <1d");
        else
            snprintf(next, next_sz, "New in %.0fd", days_to_new);
    }

    /* --- Rise / Set (cache read only; the scan runs on the poll task) --- */
    const app_config_t *cfg = app_config_get();

    time_t now;
    time(&now);

    portENTER_CRITICAL(&s_rs_mux);
    time_t rise_t = s_rs_rise, set_t = s_rs_set;
    bool   rise_v = s_rs_rise_v, set_v = s_rs_set_v;
    portEXIT_CRITICAL(&s_rs_mux);

    bool use_24h = (cfg->weather_time_format == 1);

    /* The day-offset suffix is derived from `now` on every call, so a cache up to
     * ~60 s old still renders the correct "+1"/"-1" across a local day rollover. */
    fmt_moon_event(rise, rise_sz, "Rise", rise_t, rise_v, now, use_24h);
    fmt_moon_event(set,  set_sz,  "Set",  set_t,  set_v,  now, use_24h);
}

/* ---- shared helpers ---- */

/* Sleep until the frame's next due time. For the network sources the early
 * return in net_poll_once() ("frame still fresh") must NOT restart a full
 * interval, or a page-entry/config wake could drift a frame to ~2x its
 * interval; sleep only the remaining age. Moon renders on every call, so it
 * always sleeps its full cadence. */
static uint32_t interval_cb(void *arg)
{
    image_page_t *p = (image_page_t *)arg;
    uint32_t interval = image_page_interval_ms(p);
    /* Moon renders every call; radar keeps its frames in the ring rather than
     * p->frame, so neither has a p->frame age to shorten the sleep by. */
    if (p->src == IMG_SRC_MOON || p->src == IMG_SRC_RADAR) return interval;
    int64_t age_ms = -1;
    if (xSemaphoreTake(p->frame_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (p->frame.buf) age_ms = esp_timer_get_time() / 1000 - p->frame.stamp_ms;
        xSemaphoreGive(p->frame_mux);
    }
    if (age_ms < 0) return interval;                    /* no frame: full interval (failure backoff handles retries) */
    if (age_ms >= (int64_t)interval) return 1000;       /* overdue: re-poll almost immediately */
    return interval - (uint32_t)age_ms;                 /* remaining */
}

/* One image download+decode in flight across ALL five pollers (today's single
 * goes task never ran two fetches at once; two concurrent 1 MB JPEG + 2 MB
 * decode transients would exceed the PSRAM budget and double the outbound
 * TLS sockets). A mutex is the right binary gate here (only the taker gives).
 * Created by image_page_poll_init(), which image_page_init() calls from
 * app_main (single-threaded), so no lazy-create race. The warm poller simply
 * waits, which is exactly what prefetch means. */
static SemaphoreHandle_t s_fetch_gate = NULL;

void image_page_poll_init(void)
{
    if (!s_fetch_gate) s_fetch_gate = xSemaphoreCreateMutex();
}

/* Hand a locally rendered Moon frame to the page (instant swap). */
static void moon_commit(image_page_t *p, uint16_t *img, int w, int h)
{
    image_frame_t f = {0};
    f.buf = (uint8_t *)img;
    f.w = (uint16_t)w;
    f.h = (uint16_t)h;
    f.stamp_ms = esp_timer_get_time() / 1000;
    image_page_commit_frame(p, &f, true);
}

/* ---- GOES / Solar / Custom / Radar ---- */

/* Build a RIDGE still URL (radar_map_style 0). `token` is a char[16] and
 * `frame` is 0..9, so the longest possible result is 41 + 15 + 1 + 6 + NUL = 64
 * bytes; the callers' url[RADAR_WMS_URL_MAX] (512, sized for the WMS request
 * that styles 1/2 build in the same buffer) cannot truncate and satisfies
 * -Werror=format-truncation on its own array bounds.
 *
 * HARD BAN: `frame` is an int index 0..9 and MUST STAY ONE. Never widen this
 * to a string suffix and never let a caller pass "loop": {TOKEN}_loop.gif is
 * an animated GIF that stb decodes by allocating all ten frames at once
 * (12.59 MiB + 2.83 MiB scratch, reallocated up to a 26.75 MiB double-peak =
 * intermittent OOM), and its frames are inter-frame optimised so they cannot be
 * split apart afterwards either. The numbered stills below ARE that loop, one
 * frame of memory at a time. The token half is held to A-Z0-9 by
 * radar_token_valid() (radar_play.h) so it cannot smuggle "_loop.gif?" in
 * through a query string. */
static void radar_frame_url(char *url, size_t sz, const char token[16], int frame)
{
    snprintf(url, sz, "https://radar.weather.gov/ridge/standard/%s_%d.gif", token, frame);
}

/* Time of the last successful newest-frame push, so the freshness check below
 * can skip a download. Touched only by this task. */
static int64_t s_radar_last_push_ms = 0;

/* Advertised TIME stamps of the radar layer for styles 1/2 (GeoServer WMS),
 * NEWEST FIRST, refreshed on every newest-frame fetch. Touched only by the
 * radar poll task (net_poll_once and radar_backfill run on it sequentially).
 * File-scope on purpose: 320 B does not belong on the 12288 B poller stack. */
static char s_radar_times[RADAR_WMS_TIMES_MAX][RADAR_WMS_TIME_MAX];
static int  s_radar_ntimes = 0;

/* Build the WMS GetMap URL for `token` under styles 1/2. `token` has already
 * passed image_page_radar_token() (A-Z0-9 only) and radar_wms_frame_url()
 * re-checks the alphabet, so the _loop.gif ban above cannot be bypassed here.
 * false when the token is neither a region/mosaic nor a known site (the page
 * caption names it). */
static bool radar_wms_url_for(char *url, size_t sz, const app_config_t *cfg,
                              const char *token, const char *stamp)
{
    float lat = 0.0f, lon = 0.0f;
    if (!radar_wms_region(token) && !radar_site_coords(token, &lat, &lon)) return false;
    return radar_wms_frame_url(url, sz, cfg->radar_map_style, token, lat, lon, stamp);
}

/* Refresh s_radar_times from the namespace-scoped GetCapabilities document.
 * Blind time stepping does not work (the newest frame lags wall clock by a
 * variable amount and out-of-range TIME returns a valid near-black GIF), so
 * the advertised list is the only safe source of stamps. Empty on any failure:
 * the newest fetch then omits TIME (server default = newest) and backfill
 * stops. KHDC is the standing case: its namespace GetCapabilities advertises
 * zero layers (so no time list) although its GetMap serves a live picture, so
 * on styles 1/2 it shows the newest picture only, with no history. */
static void radar_wms_refresh_times(const char *token)
{
    char url[128];
    char layer[32];
    s_radar_ntimes = 0;
    if (!radar_wms_caps_url(url, sizeof(url), token) ||
        !radar_wms_layer_name(layer, sizeof(layer), token)) return;
    http_fetch_opts_t opts = {
        .timeout_ms = 10000,
        .use_tls_bundle = true,
        .max_response_bytes = 65536,
        .user_agent = "NINA-Display/1.0 (ESP32-P4 dashboard)",
    };
    char *body = NULL;
    size_t len = 0;
    if (http_fetch_text(url, &opts, &body, &len) != ESP_OK) {
        ESP_LOGW(TAG, "radar caps fetch failed for %s; newest only", token);
        return;
    }
    s_radar_ntimes = radar_wms_parse_times(body, len, layer, s_radar_times, RADAR_WMS_TIMES_MAX);
    heap_caps_free(body);
    if (s_radar_ntimes == 0) ESP_LOGW(TAG, "no advertised radar times for %s; newest only", token);
}

/* Rebuild the history behind the newest frame after a page activation: fetch
 * {TOKEN}_1.gif upward (style 0) or the advertised WMS stamps from the second
 * newest down (styles 1/2) and APPEND each at the tail, so the ring stays
 * newest-first without ever reordering. Frames are STAGGERED ~1 s apart rather
 * than run back to back: this board glitches the panel under sustained radio
 * transmission (see wifi_max_tx_dbm), and nine rapid TLS downloads are exactly
 * that profile. Stops early when the ring fills, a fetch fails (older frames
 * are optional), or the page is left mid-backfill. */
#define RADAR_BACKFILL_GAP_MS 1000

static void radar_backfill(image_page_t *p, const app_config_t *cfg)
{
    int cap = image_page_radar_capacity();
    if (cap <= 1) return;                       /* radar_frames == 1: a still, no ring */

    /* Generation FIRST, token second: the config write lands before the ring
     * reset bumps the generation, so this order can never pair an old token
     * with the new generation (see radar_frame_is_stale in radar_play.h). */
    uint32_t gen = image_page_radar_gen();
    char token[16];
    image_page_radar_token(cfg, token, sizeof(token));
    uint8_t style = cfg->radar_map_style;

    for (int i = 1; i < cap; i++) {
        if (!atomic_load(&p->poll_gate)) break;             /* page left / un-warmed */
        if (image_page_radar_count() >= cap) break;         /* ring full */
        vTaskDelay(pdMS_TO_TICKS(RADAR_BACKFILL_GAP_MS));
        /* Superseded mid-backfill (region/frame-count/crop change, page leave).
         * image_page_radar_add() would reject these frames anyway; bailing here
         * stops us DOWNLOADING them. This loop lives ~9 s, so without it a
         * region switch costs up to eight more 32 KB TLS fetches for a region
         * nobody is looking at — and sustained radio transmission is what
         * glitches this panel (see wifi_max_tx_dbm). Placed after the delay so
         * it gates every fetch, including the first. */
        if (radar_frame_is_stale(gen, image_page_radar_gen())) {
            ESP_LOGI(TAG, "radar backfill superseded at frame %d", i);
            return;
        }

        /* Service a crop / dark-mode / Red-Night change BETWEEN frames rather
         * than after the whole backfill. This loop lives ~9 s for ten frames and
         * far longer when fetches are slow; the request used to wait for the
         * next net_poll_once(), which is why a logged change took 18.4 s to
         * reach the ring. Here it converges within one backfill gap.
         *
         * No second flag and no early exit: this is the SAME request the poll
         * entry consumes, taken with an atomic exchange so it cannot be serviced
         * twice or dropped, and the loop simply continues afterwards — every
         * remaining frame is still fetched, in order, and each is baked live
         * under the settings that just won. Safe to call from here: it runs on
         * this task, and no insert is in flight at this point in the loop. */
        image_page_radar_retransform_if_requested(p);

        char url[RADAR_WMS_URL_MAX];
        if (style == 0) {
            radar_frame_url(url, sizeof(url), token, i);
        } else {
            /* Styles 1/2 index the advertised stamp list newest-first, so
             * frame i is the i-th newest -- what RIDGE's _i.gif means on
             * style 0. No more stamps (KHDC advertises none, parse failure,
             * short list) or an unknown token: the history simply ends here. */
            if (i >= s_radar_ntimes) break;
            if (!radar_wms_url_for(url, sizeof(url), cfg, token, s_radar_times[i])) break;
        }

        image_frame_t old = {0};
        uint8_t *src = NULL;
        size_t   src_len = 0;
        xSemaphoreTake(s_fetch_gate, portMAX_DELAY);
        esp_err_t e = image_fetch_custom_retain(url, &old, &src, &src_len);
        xSemaphoreGive(s_fetch_gate);
        if (e != ESP_OK) {
            if (old.buf) heap_caps_free(old.buf);
            /* src is NULL on every error return (see image_fetch_custom_retain). */
            ESP_LOGW(TAG, "radar backfill stopped at frame %d", i);
            break;
        }
        /* Takes both buffers, accepted or rejected. */
        image_page_radar_add(p, &old, false, gen, src, src_len);   /* append at the tail */
    }
    ESP_LOGI(TAG, "radar ring: %d/%d frames", image_page_radar_count(), cap);
}

static bool net_poll_once(void *arg)
{
    image_page_t *p = (image_page_t *)arg;
    const app_config_t *cfg = app_config_get();
    bool manual = atomic_exchange(&p->manual_fetch, false);

    /* Radar, in this order and BEFORE anything can insert a frame this pass:
     *   1. a ring teardown image_page_radar_invalidate() had to defer because the
     *      display lock was busy (the generation bump already landed, so the ring
     *      is only stale, never mixed) — running it here, ahead of the fetch
     *      below, is what keeps a frame under the NEW settings from joining old
     *      ones. A no-op unless a lock timeout actually happened;
     *   2. a pending crop / dark-mode / Red-Night change, which re-derives the
     *      ring from each frame's retained compressed bytes. Runs BEFORE the
     *      freshness early-return below (a full, fresh ring is exactly when it is
     *      needed) and costs no network.
     * Both are no-ops for the other four sources. */
    if (p->src == IMG_SRC_RADAR) {
        image_page_radar_reset_if_requested(p);
        image_page_radar_retransform_if_requested(p);
    }

    /* A frame that is still within its interval (a warm/prefetched frame on
     * page entry, or a wake that was not a manual/config request) needs no
     * download. */
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t age_ms = 0;
    bool have = false;
    if (p->src == IMG_SRC_RADAR) {
        /* Radar's frames live in the ring, not p->frame. "Have" means the ring
         * is FULL: a partly-built ring must keep polling so the backfill below
         * gets its chance, whatever the interval says. */
        have = image_page_radar_count() >= image_page_radar_capacity();
        age_ms = now_ms - s_radar_last_push_ms;
    } else if (xSemaphoreTake(p->frame_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        have = (p->frame.buf != NULL);
        age_ms = now_ms - p->frame.stamp_ms;
        xSemaphoreGive(p->frame_mux);
    }
    if (have && !manual && age_ms < (int64_t)image_page_interval_ms(p)) return true;

    /* Loading overlay: manual request, or nothing on screen yet (page entry
     * without a warm frame). Periodic refreshes over a good frame never flash it. */
    char label[48];
    image_page_label(p, label, sizeof(label));
    bool show_wait = false;
    if (atomic_load(&p->active) && bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        if (manual || !image_page_has_image(p)) {
            nina_wait_overlay_show("Loading image...", label[0] ? label : NULL);
            nina_wait_overlay_set_progress(-1);
            show_wait = true;
        }
        bsp_display_unlock();
    }

    image_frame_t fresh = {0};
    esp_err_t err;
    /* Ring generation for a radar fetch, captured BEFORE the token is resolved
     * below and handed to image_page_radar_add() so a frame that finishes
     * downloading after a region switch is rejected instead of mixed into the
     * new ring. Unused by the other four sources. */
    uint32_t radar_gen = image_page_radar_gen();
    /* Radar retains its compressed source bytes so the ring can be re-derived
     * locally on a crop/dark-mode/theme change. Ownership passes to
     * image_page_radar_add(); NULL on every non-radar path and on any error. */
    uint8_t *radar_src = NULL;
    size_t   radar_src_len = 0;
    /* Serialize the fetch+decode across the five pollers (see s_fetch_gate).
     * A page that was left/un-warmed while we waited needs no extra test here:
     * image_page_commit_frame() retains the frame either way, and the resident
     * cap frees it if the budget is exceeded. */
    xSemaphoreTake(s_fetch_gate, portMAX_DELAY);
    switch (p->src) {
        case IMG_SRC_GOES:
            err = cfg->goes_region[0] ? image_fetch_goes(cfg->goes_region, &fresh) : ESP_FAIL;
            break;
        case IMG_SRC_SOLAR:
            err = image_fetch_solar(cfg->solar_band, &fresh);
            break;
        case IMG_SRC_RADAR: {
            /* Token resolved per fetch and never written back to config (a poll
             * task must not persist config). Frame 0 is the newest still. */
            char token[16];
            image_page_radar_token(cfg, token, sizeof(token));
            char url[RADAR_WMS_URL_MAX];
            if (cfg->radar_map_style == 0) {
                radar_frame_url(url, sizeof(url), token, 0);
                err = image_fetch_custom_retain(url, &fresh, &radar_src, &radar_src_len);
                break;
            }
            /* Styles 1/2: the caps fetch is a network op, so it stays inside
             * s_fetch_gate with the frame fetch. Newest = advertised stamp [0],
             * or the server default (= newest) when none were advertised. The
             * token passed image_page_radar_token() and radar_wms_frame_url()
             * re-checks its alphabet, so the _loop.gif ban holds on this path. */
            radar_wms_refresh_times(token);
            if (!radar_wms_url_for(url, sizeof(url), cfg, token,
                                   s_radar_ntimes > 0 ? s_radar_times[0] : NULL)) {
                strlcpy(fresh.error_msg, "Unknown radar area", sizeof(fresh.error_msg));
                err = ESP_ERR_INVALID_ARG;
            } else {
                err = image_fetch_custom_retain(url, &fresh, &radar_src, &radar_src_len);
            }
            break;
        }
        default:   /* IMG_SRC_CUSTOM */
            /* image_fetch_custom() rejects an empty URL without filling in a
             * reason, so name it here; image_page_set_error() below stores it
             * and the page renders it as the caption. */
            if (cfg->custom_image_url[0] == '\0') {
                strlcpy(fresh.error_msg, "No URL configured", sizeof(fresh.error_msg));
                err = ESP_ERR_INVALID_ARG;
            } else {
                err = image_fetch_custom(cfg->custom_image_url, &fresh);
            }
            break;
    }
    xSemaphoreGive(s_fetch_gate);

    if (err == ESP_OK) {
        if (p->src == IMG_SRC_RADAR) {
            /* Newest frame at the head; a re-served identical still is deduped
             * inside _add. Only then rebuild the history (outside the fetch
             * gate, which the backfill re-takes per download). */
            image_page_radar_add(p, &fresh, true, radar_gen, radar_src, radar_src_len);
            /* Radar never goes through image_page_commit_frame(), which is what
             * clears p->frame.error_msg for every other source (it assigns the
             * whole zeroed `fresh`). Without this, one transient failure latches
             * the error caption forever: image_page_render_frame() checks the
             * error branch BEFORE the ring branch, so every later activation
             * repaints the stale reason until the first frame lands. */
            image_page_set_error(p, "");
            s_radar_last_push_ms = esp_timer_get_time() / 1000;
            if (image_page_radar_backfill_take()) radar_backfill(p, cfg);
        } else {
            image_page_commit_frame(p, &fresh, false);
        }
        return true;
    }
    image_page_set_error(p, fresh.error_msg[0] ? fresh.error_msg : "Fetch failed");
    if (show_wait) {
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            nina_wait_overlay_hide();
            bsp_display_unlock();
        }
        nina_toast_show(TOAST_WARNING, "Failed to load image");
    }
    return false;
}

/* Radar park point. The other consumption site for a deferred ring teardown is
 * the top of net_poll_once(), which only runs while the page is gated ON — and
 * the invalidate that had to defer often comes from image_page_disable(), which
 * closes that gate immediately. Without this hook a lock timeout on the disable
 * path would hold the whole ring (up to ~6.6 MB of PSRAM) until the page was
 * enabled again. Blocking here is free: nothing renders on this task while
 * parked, and no insert can be in flight (image_page_radar_add runs only on
 * this task, and only from net_poll_once). */
static void radar_on_park(void *arg)
{
    image_page_radar_reset_if_requested((image_page_t *)arg);
}

/* ---- Moon ---- */

/* Safe release point: nothing renders on this task while parked. The gate
 * also closes for ota_in_progress while the Moon page is still ACTIVE; in
 * that case the LVGL descriptor may still borrow s_ppa_out[] (only
 * image_page_release_lvgl on page HIDE drops it), so the drag/PPA scratch has
 * to stay. The tgx texture is never handed to LVGL, so it is dropped either
 * way -- and it MUST be, because on_park fires only on running->parked, so a
 * page leave that happens later in the same OTA park would never re-fire it
 * and the ~1 MB would stay allocated for the whole update. */
static void moon_on_park(void *arg)
{
    image_page_t *p = (image_page_t *)arg;
    /* OTA park on a visible Moon page: free the texture (moon_sphere_init() at
     * the top of moon_poll_once re-decodes it lazily), keep the scratch. */
    if (atomic_load(&p->active)) { moon_sphere_deinit(); return; }
    moon_sphere_deinit();       /* ~1MB cached lunar texture */
    moon_drag_buffers_free();   /* drag color/z + PPA ping-pong */
    moon_drag_reset();
    s_moon_light_mix = 0.0f;
    ESP_LOGI(TAG, "Moon page parked: freed texture + drag buffers");
}

static bool moon_poll_once(void *arg)
{
    image_page_t *p = (image_page_t *)arg;
    const app_config_t *cfg = app_config_get();

    /* The Moon renders locally; a manual request just means "render now". */
    atomic_exchange(&p->manual_fetch, false);

    time_t now;
    time(&now);
    bool time_valid = (now > (time_t)1577836800);   /* >= 2020-01-01 UTC */
    bool moon_ready = moon_sphere_init();
    if (!time_valid || !moon_ready) return true;    /* interval_cb sleeps 3 s while the clock is invalid */

    double lat = cfg->moon_lat, lon = cfg->moon_lon;
    if (lat == 0.0 && lon == 0.0) { lat = cfg->weather_lat; lon = cfg->weather_lon; }
    moon_state_t live;
    moon_compute(now, lat, lon, &live);
    /* Keep the overlay's rise/set cache warm here - Core 0, no display lock
     * held. moon_overlay_info() must never run this scan itself. */
    moon_rise_set_refresh();

    /* Interactive drag-to-rotate: while a finger is down OR the disc is
     * still easing back to the live sky orientation after release, render
     * small frames in realtime and PPA hardware-upscale them to fill the
     * panel. The touch handlers in ui/nina_image_page.c notify this task on
     * press/release, so the outer ulTaskNotifyTake wakes us; this inner
     * loop owns the realtime frames and SELF-SPINS (it does not wait on
     * notifications) until moon_drag_settled() — i.e. the finger is up AND
     * the eased orientation is home — then falls through to the crisp
     * full-res resting render (crossfaded in for a smooth handoff).
     *
     * Architecture (per-frame, zero heap alloc):
     *   1. ease current orientation toward the finger target (per-frame
     *      exp while dragging; TIME-based ease-out during the settle so it
     *      reads smooth at any framerate instead of jumping home);
     *   2. render the sphere at the single touch size (240px, 96x48
     *      tessellation) into PERSISTENT scratch (no alloc);
     *   3. PPA hardware-upscale 240->720 (exact 3.0x) into a ping-pong
     *      output buffer, then show_borrowed it at scale 1.0 (no copy),
     *      taking the display lock only around the LVGL swap.
     * The render + PPA run OUTSIDE the display lock so the UI task is
     * blocked for the minimum time. Every drag frame (active, hold, and
     * settle) uses the configured drag lighting mode; EXPLORE is
     * crossfaded in over MOON_LIGHT_FADE_MS on touch-down and back out
     * once the disc is home (see step 2 below), so the final crisp
     * native-720 resting frame (true phase) matches the last drag frame.
     * If PPA or the buffers are unavailable, the loop falls
     * back to a software-scaled render so it degrades instead of crashing. */
    /* Whether the drag loop body actually ran. Sampling moon_drag_settled()
     * up front is unreliable: a press+release can complete before this task
     * wakes, leaving settled() already true so the loop never executes. We
     * set this inside the loop instead, so the post-settle resting commit
     * fires whenever a drag frame was actually put on screen. Declared
     * OUTSIDE the for(;;) below so it intentionally persists across outer
     * iterations: once any drag frame has run it stays true, so the grace
     * window fires after every drag including re-grabs. */
    bool ran_drag = false;
    /* Set when the inner render loop breaks because a free-spin hold is
     * active (finger up, disc held at its spun orientation). Drives the
     * dedicated hold-wait block below instead of the normal resting commit.
     * Re-evaluated each outer iteration. */
    bool freespin_hold = false;
    if (!moon_drag_settled()) {
        /* Lazy one-time allocation of the drag scratch + PPA output
         * (PSRAM, 128-byte aligned for the cache line). The color buffer
         * is 240+GUARD_ROWS tall (the guard rows absorb the SRM bottom-edge
         * upscale over-read; see the buffer-block comment above); the zbuf
         * is plain 240px (not DMA-read by PPA). The two 720x720 PPA-output
         * buffers ping-pong. All freed on page leave via
         * moon_drag_buffers_free(). This runs ONCE on entry (before the
         * for(;;) below); the inner "if any buffer NULL" guard makes it a
         * no-op when the buffers already exist, so a re-grab that loops back
         * into the drag while-loop never re-allocates. */
        if (!s_drag_color || !s_drag_zbuf || !s_ppa_out[0] || !s_ppa_out[1]) {
            size_t row_bytes  = (size_t)MOON_DRAG_SZ_TOUCH * 2;
            size_t color_sz   = ((size_t)MOON_DRAG_SZ_TOUCH + MOON_DRAG_GUARD_ROWS) * row_bytes;
            color_sz = (color_sz + 127) & ~(size_t)127;   /* 248*480=119040, already 128-aligned */
            size_t zbuf_sz    = (size_t)MOON_DRAG_SZ_TOUCH * row_bytes;
            zbuf_sz  = (zbuf_sz + 127) & ~(size_t)127;
            size_t out_sz     = (size_t)SCREEN_SIZE * SCREEN_SIZE * 2;   /* 720*720*2 = 128-aligned */
            moon_drag_buffers_free();   /* drop any partial alloc first */
            s_drag_color = (uint16_t *)heap_caps_aligned_alloc(128, color_sz, MALLOC_CAP_SPIRAM);
            s_drag_zbuf  = (uint16_t *)heap_caps_aligned_alloc(128, zbuf_sz, MALLOC_CAP_SPIRAM);
            s_ppa_out[0] = (uint16_t *)heap_caps_aligned_alloc(128, out_sz, MALLOC_CAP_SPIRAM);
            s_ppa_out[1] = (uint16_t *)heap_caps_aligned_alloc(128, out_sz, MALLOC_CAP_SPIRAM);
            if (!s_drag_color || !s_drag_zbuf || !s_ppa_out[0] || !s_ppa_out[1]) {
                ESP_LOGE(TAG, "moon drag buffer alloc failed; falling back to software scale");
                moon_drag_buffers_free();   /* drop the partial set; loop uses the SW fallback */
            } else {
                /* Zero the guard rows below the 240 valid rows and flush
                 * them to PSRAM ONCE (C2M) so the SRM bottom-edge over-read
                 * samples our own black memory, not adjacent heap. The
                 * per-frame render writes only rows 0..239, so the guard
                 * rows stay zeroed and this never repeats. Offset/length
                 * are both 128-aligned (115200 / 3840). */
                uint8_t *guard = (uint8_t *)s_drag_color +
                                 (size_t)MOON_DRAG_SZ_TOUCH * row_bytes;
                size_t   guard_bytes = (size_t)MOON_DRAG_GUARD_ROWS * row_bytes;
                memset(guard, 0, guard_bytes);
                esp_cache_msync(guard, guard_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            }
        }
    }

    /* Outer loop wraps the drag/settle while-loop and a post-settle GRACE
     * window. The grace hold (MOON_DRAG_REST_GRACE_MS) makes the reset less
     * eager: after the disc eases home we wait briefly, and if the user
     * starts a NEW touch within that window we `continue` to re-enter the
     * drag while-loop and track the finger immediately, SKIPPING the
     * expensive crisp resting render + crossfade entirely. Only when the
     * full grace window elapses with no re-touch do we break out to commit
     * the resting frame (genuine rest -> smooth eased settle + crossfade is
     * preserved). A new touch makes moon_drag_settled() false again, so
     * re-entering the while-loop resumes tracking with no special-casing. */
    for (;;) {
    /* Seed the per-frame dt clock for the time-based settle ease. Keep
     * this immediately before the loop (after the scratch alloc above) so
     * the first settle frame's dt is a true frame interval and never folds
     * in one-time alloc latency. Re-seeded each outer iteration so a
     * re-touch's first settle frame gets a true dt. */
    int64_t prev_frame_us = esp_timer_get_time();
    freespin_hold = false;   /* re-evaluated for this outer iteration */
    /* Loop while the disc is away from home OR the explore lighting is
     * still fading out: once the disc is home, EXPLORE mode keeps
     * rendering a few frames at yaw=pitch=0 with the mix easing to 0 so
     * the lit->true-phase handoff is a crossfade, not a switch. The
     * resting render below is then identical to the last frame. */
    while (!moon_drag_settled() || s_moon_light_mix > 0.0f) {
        if (!atomic_load(&p->active)) break;
        ran_drag = true;   /* a drag frame is about to be shown */
        int64_t frame_t0 = esp_timer_get_time();
        float dt_ms = (float)(frame_t0 - prev_frame_us) / 1000.0f;
        prev_frame_us = frame_t0;

        /* 1. Ease current orientation toward the finger target. While the
         * finger is down, use the responsive per-frame alpha. After release
         * (settle), derive a framerate-independent ease-out alpha from the
         * frame dt so the snap-back always takes ~MOON_DRAG_SETTLE_MS and
         * reads smooth even at ~14fps (a fixed per-frame alpha would finish
         * in 2-3 frames and look like a jump). */
        bool active = moon_drag_active();
        float alpha;
        if (active) {
            alpha = MOON_DRAG_EASE_A;
        } else {
            /* tau so ~95% of the distance is covered in SETTLE_MS:
             * alpha = 1 - exp(-dt/tau), tau = SETTLE_MS/3. */
            float tau   = (float)MOON_DRAG_SETTLE_MS / 3.0f;
            alpha = 1.0f - expf(-dt_ms / tau);
            if (alpha < 0.02f) alpha = 0.02f;   /* never fully stall */
            if (alpha > 1.0f)  alpha = 1.0f;
        }
        moon_drag_advance(alpha);
        float yaw, pitch; moon_drag_get(&yaw, &pitch);

        /* 2. Lighting for this frame. The configured drag mode applies to
         * EVERY frame of the interaction (finger down, follow-through, hold,
         * ease home). EXPLORE is not switched on/off but CROSSFADED: the
         * mix ramps 0->1 over MOON_LIGHT_FADE_MS on touch-down, holds at 1
         * while the disc is away from home (so it stays fully lit through
         * release, hold and the return), and ramps back to 0 once the disc
         * is home, dissolving the terminator back in before the resting
         * commit. TRUE_PHASE and SURFACE_LOCKED render their own mode with
         * mix 0 (SURFACE_LOCKED's sun = R_drag*R_sky*sun_body equals true
         * phase at yaw=pitch=0, so it converges with no pop). The render
         * SIZE is the single touch size (240) for the whole interaction —
         * active and settle — because 240->720 is an exact 3.0x PPA ratio. */
        moon_light_mode_t cfg_light = (moon_light_mode_t)cfg->moon_drag_light_mode;
        bool  explore  = (cfg_light == MOON_LIGHT_EXPLORE);
        bool  want_lit = explore && !moon_drag_settled();
        float mix_step = dt_ms / (float)MOON_LIGHT_FADE_MS;
        s_moon_light_mix += want_lit ? mix_step : -mix_step;
        if (s_moon_light_mix < 0.0f) s_moon_light_mix = 0.0f;
        if (s_moon_light_mix > 1.0f) s_moon_light_mix = 1.0f;
        /* Base mode for the mixed render: EXPLORE fades over TRUE_PHASE. */
        moon_light_mode_t light = explore ? MOON_LIGHT_TRUE_PHASE : cfg_light;

        /* Free-spin hold: after a rotate-release in free-spin mode the
         * disc holds its spun orientation, so moon_drag_settled() never
         * becomes true and this loop would busy-render an identical frame
         * for the whole hold window. Wait until the eased CURRENT has
         * actually converged onto the spun TARGET (moon_drag_advance, run
         * above each frame, is still easing it there) before breaking;
         * otherwise we would lock in a half-eased orientation as the crisp
         * held frame and the disc would visibly snap after lift. In EXPLORE
         * also wait for the lighting fade-in to finish, since the crisp held
         * frame is rendered at full explore (mix 1) and must not pop. Once
         * converged, break to the hold-wait below (which sleeps instead of
         * re-rendering). */
        if (!active && moon_drag_freespin_converged() &&
            (!explore || s_moon_light_mix >= 1.0f)) {
            freespin_hold = true;
            break;
        }

        bool shown = false;
        if (s_drag_color && s_drag_zbuf && s_ppa_out[0] && s_ppa_out[1]) {
            /* Render 240px into persistent scratch — NO per-frame alloc. */
            uint16_t *fimg = moon_sphere_render_into(
                MOON_DRAG_SZ_TOUCH, MOON_DRAG_SZ_TOUCH, &live,
                MOON_DRAG_SECTORS, MOON_DRAG_STACKS, cfg->moon_bg_style,
                yaw, pitch, light, s_moon_light_mix, s_drag_color, s_drag_zbuf);
            if (fimg) {
                /* Red Night: recolour the 240px SOURCE, not the 720 PPA
                 * output. The remap is per-pixel and the upscale linear, so
                 * they commute — same result for 1/9th the pixels (57,600 vs
                 * 518,400) every drag frame. The scratch is fully re-rendered
                 * each frame, so this never compounds. */
                bool red = image_red_remap_active();
                if (red) {
                    image_red_remap_rgb565_force(
                        fimg, (size_t)MOON_DRAG_SZ_TOUCH * MOON_DRAG_SZ_TOUCH);
                }

                /* PPA hardware-upscale 240->720 (exact 3.0x) into the
                 * ping-pong output buffer NOT currently on screen. No
                 * memset (every output pixel is written) and the blocking
                 * PPA handles cache coherency both ways, so the buffer is
                 * coherent for LVGL read on return. */
                int ping = s_ppa_ping;
                uint8_t *out = ppa_scale_rgb565_into_noclear(
                    (const uint8_t *)fimg, MOON_DRAG_SZ_TOUCH, MOON_DRAG_SZ_TOUCH,
                    MOON_DRAG_SZ_TOUCH /* stride in pixels */,
                    SCREEN_SIZE, SCREEN_SIZE,
                    (uint8_t *)s_ppa_out[ping],
                    (size_t)SCREEN_SIZE * SCREEN_SIZE * 2, NULL);
                if (out) {
                    /* Point the LVGL descriptor straight at the 720 buffer
                     * (no copy) at scale 1.0. Lock held only around the
                     * swap. The ping-pong guarantees we never overwrite the
                     * buffer LVGL is flushing. */
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        image_page_show_borrowed(p, s_ppa_out[ping], SCREEN_SIZE, SCREEN_SIZE);
                        bsp_display_unlock();
                    }
                    s_ppa_ping ^= 1;   /* flip: PPA writes the other buffer next frame */
                    shown = true;
                }
                /* PPA failed: fall through to the software-scale fallback
                 * below using the SAME already-rendered 240px scratch.
                 * Skipped under red night — the scratch is already remapped
                 * and show_scaled() remaps its own copy again, which would
                 * double-darken it; leaving `shown` false hands the frame to
                 * the fresh-render fallback below, which remaps exactly once. */
                if (!shown && !red && bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    image_page_show_scaled(p, fimg, MOON_DRAG_SZ_TOUCH, MOON_DRAG_SZ_TOUCH);
                    bsp_display_unlock();
                    shown = true;
                }
            }
        }

        if (!shown) {
            /* Buffers unavailable (alloc failed): render to a fresh PSRAM
             * buffer and let image_page_commit_frame() software-scale it.
             * Degrades gracefully. NOTE: unlike the PPA happy path, this
             * exceptional fallback does ~2 allocs/frame (the fresh
             * moon_sphere_render_ex color/z here, plus update()'s owned-copy
             * alloc); the "zero per-frame heap alloc" guarantee applies only
             * to the PPA path, not this degraded fallback. render_ex has no
             * mix input, so this path renders the configured mode directly
             * (lighting switches instead of crossfading; acceptable here). */
            uint16_t *fimg = moon_sphere_render_ex(MOON_DRAG_SZ_TOUCH, MOON_DRAG_SZ_TOUCH, &live,
                                                   MOON_DRAG_SECTORS, MOON_DRAG_STACKS,
                                                   cfg->moon_bg_style, yaw, pitch, cfg_light);
            if (fimg) {
                moon_commit(p, fimg, MOON_DRAG_SZ_TOUCH, MOON_DRAG_SZ_TOUCH);
            }
        }

        /* Delay only the remainder to hit the target frame period. If the
         * frame overran (the usual case; the PPA upscale is cheap but the
         * 240px tgx render dominates), just yield so we don't starve
         * idle/UI but also don't add latency. */
        int64_t spent = esp_timer_get_time() - frame_t0;
        int64_t remain_us = (int64_t)MOON_DRAG_FRAME_US - spent;
        if (remain_us > 1000) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)(remain_us / 1000)));
        } else {
            vTaskDelay(1);
        }
    }

    /* FREE-SPIN HOLD. In free-spin mode (moon_spin_mode == 1) a
     * rotate-release leaves the disc at its spun orientation instead of
     * snapping home. Render that held orientation ONCE as a crisp native-720
     * frame (the inner loop only ever showed the 240px PPA-upscaled version),
     * then sleep-poll — NOT busy-render — until either a new finger-down
     * re-enters the drag loop (cancels the pending return) or the configured
     * moon_spin_return_s elapses, at which point we trigger the eased return
     * home and `continue` so the inner settle loop runs the snap-back +
     * resting commit exactly as the rubber-band path does. */
    if (freespin_hold) {
        /* One crisp held-orientation frame at native 720. moon_sphere_render_ex
         * owns its own PSRAM buffer; the commit takes ownership. */
        float hy, hp; moon_drag_get(&hy, &hp);
        uint16_t *hold_img = moon_sphere_render_ex(SCREEN_SIZE, SCREEN_SIZE, &live,
                                                   96, 48, cfg->moon_bg_style,
                                                   hy, hp, (moon_light_mode_t)cfg->moon_drag_light_mode);
        if (hold_img && atomic_load(&p->active) && !moon_drag_active()) {
            moon_commit(p, hold_img, SCREEN_SIZE, SCREEN_SIZE);
        } else if (hold_img) {
            heap_caps_free(hold_img);
        }

        /* Sleep-poll the hold window. The configured seconds are read each
         * iteration so a live web-UI change takes effect within one poll step. */
        for (;;) {
            if (!atomic_load(&p->active)) break;
            if (moon_drag_active()) break;   /* re-touch: outer continue re-enters the drag loop */
            /* Mode switched to rubber band mid-hold, or the hold was cleared:
             * resolve by snapping home (acceptable per spec). */
            if (!moon_drag_freespin_pending()) {
                moon_drag_trigger_return();
                break;
            }
            if (moon_drag_freespin_elapsed(cfg->moon_spin_return_s)) {
                moon_drag_trigger_return();   /* target -> 0: ease home next iteration */
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        /* Re-enter immediately: a re-touch resumes the drag, otherwise the
         * snap-back eases home on the next call. */
        image_page_wake(p);
        return true;
    }

    /* GRACE / idle hold before committing the resting render. Poll in
     * small steps; if a new touch begins, restart the outer loop so the
     * drag while-loop re-tracks the finger immediately (no rest render,
     * no crossfade). Bail the whole interaction if the page/source changed.
     * Only a fully-elapsed grace window (no re-touch) falls through to the
     * resting commit below. */
    bool regrabbed = false;
    if (ran_drag) {
        int waited_ms = 0;
        while (waited_ms < MOON_DRAG_REST_GRACE_MS) {
            if (!atomic_load(&p->active)) break;
            if (moon_drag_active()) { regrabbed = true; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
            waited_ms += 20;
        }
    }
    if (regrabbed) {
        image_page_wake(p);   /* re-enter the drag while-loop immediately */
        return true;
    }
    break;                     /* genuine rest: commit the resting frame */
    } /* end outer for(;;) */

    /* Drag settled: the last 240px settle frame (home orientation, explore
     * mix faded to 0 = true phase, PPA-upscaled) is still on screen. The full-res resting render below commits an
     * OWNED native-720 frame and crossfades it in over that settle frame
     * for a smooth sharpen-up. The render scratch / PPA buffers are freed
     * on page leave, not here, so the next drag reuses them. */

    /* One-shot tap animation: ~4s eased sweep through a full synodic
     * cycle plus a full bright-limb spin, rendered at reduced size for
     * smoothness. Consume the request atomically so a single tap fires
     * once. Both phase and orientation are periodic over the sweep, so
     * t=1 lands back on the live values with no visible jump. */
    if (atomic_exchange(&moon_anim_request, false)) {
        /* tgx tap-animation is a later phase; consume the tap (above)
         * and otherwise no-op, so the resting tgx frame renders below. */
    }

    /* Normal full-res render of the live current phase. Runs whether or
     * not an animation played; the caption reads the resting state.
     * Rendered at NATIVE 720 so it displays 1:1 (no software scale) and is
     * the sharpest possible resting frame; the ~297ms cost is a one-shot at
     * rest (not per-frame), so it is fine. update() copies it into an owned
     * buffer and crossfades it in at scale 1.0. */
    s_moon_state = live;
    const int MOON_SZ = SCREEN_SIZE;

    /* ABORT-IF-TOUCH-RESUMED backstop. The grace window above makes this
     * rare, but a touch can land right at the grace boundary, AFTER we
     * broke out of the outer loop. A single moon_sphere_render(720) blocks
     * ~297ms and cannot be interrupted mid-call, so the guard MUST be
     * BEFORE it: if a finger is down now, skip the resting render + crossfade
     * commit entirely and `continue` the task loop. moon_drag_active() makes
     * moon_drag_settled() false, so re-entering the moon block immediately
     * re-enters the drag loop and tracks the finger with no blocking stall
     * and no dropped frame. */
    if (moon_drag_active()) {
        image_page_wake(p);
        return true;
    }

    /* Render with tgx and log timing. When debug_mode is on, also
     * sweep candidate sizes so the size/fps tradeoff is visible on
     * serial for evaluation. */
    if (cfg->debug_mode && !moon_drag_active()) {
        const int sizes[3] = {240, 300, 400};
        for (int si = 0; si < 3; si++) {
            int64_t te0 = esp_timer_get_time();
            uint16_t *tmp = moon_sphere_render(sizes[si], sizes[si], &live, 96, 48, cfg->moon_bg_style);
            int64_t te = esp_timer_get_time() - te0;
            ESP_LOGI(TAG, "tgx moon %dx%d render %lld ms", sizes[si], sizes[si], te/1000);
            if (tmp) heap_caps_free(tmp);
        }
    }
    /* Spec decision 2: at most one image download+decode in flight. The 720px
     * resting render allocates ~1 MB colour + ~1 MB z, so it must not overlap a
     * GOES/Solar/Custom fetch+decode. The gate covers ONLY the render (the
     * memory-heavy part) and is given back before the commit, symmetric with
     * net_poll_once, so it never spans the display lock inside commit_frame.
     * No return/continue between take and give. (Drag frames are 240px and
     * stay outside the gate.) */
    xSemaphoreTake(s_fetch_gate, portMAX_DELAY);
    int64_t t0 = esp_timer_get_time();
    uint16_t *img = moon_sphere_render(MOON_SZ, MOON_SZ, &live, 96, 48, cfg->moon_bg_style);
    ESP_LOGI(TAG, "tgx moon %dx%d render %lld ms", MOON_SZ, MOON_SZ, (esp_timer_get_time()-t0)/1000);
    xSemaphoreGive(s_fetch_gate);
    if (img) {
        /* Commit the OWNED native-720 resting frame with force=true so it
         * swaps in immediately (bypassing the newer-stamp gate) rather than
         * waiting on the periodic UI cadence: config-driven re-renders (flip /
         * background / orientation toggles from the web UI, which wake this
         * poller) appear as soon as the render completes, and it still replaces
         * the post-drag 240px settle frame whose equal-millisecond stamp the
         * gate could otherwise tie on and skip. The swap is instant (matching
         * every other moon frame) so there is no midpoint brightness dip.
         * commit_frame renders only while the page is active and frees the
         * buffer itself if the gate closed while we rendered. */
        moon_commit(p, img, MOON_SZ, MOON_SZ);
    }

    return true;
}

/* ---- task entry ---- */

void image_page_poll_task(void *arg)
{
    image_page_t *p = (image_page_t *)arg;
    bool is_moon = (p->src == IMG_SRC_MOON);
    ESP_LOGI(TAG, "%s poll task started", p->name);

    /* Park hooks: the Moon releases its texture and drag scratch; radar services
     * a ring teardown that could not get the display lock (see radar_on_park).
     * The other three sources hold nothing that a park should release. */
    void (*park_cb)(void *) = NULL;
    if (is_moon)                        park_cb = moon_on_park;
    else if (p->src == IMG_SRC_RADAR)   park_cb = radar_on_park;

    /* Network sources: a failed first fetch on page entry (transient DNS/TLS)
     * must not leave the page black for a full 5-120 min interval; the spine's
     * failure backoff retries at 15 s, doubling to 5 min. The Moon never fails
     * (local render), so it keeps backoff off. */
    poll_loop_spec_t spec = {
        .name = p->name,
        .wifi_group = is_moon ? NULL : s_wifi_event_group,   /* the Moon needs no network */
        .wifi_bits = WIFI_CONNECTED_BIT,
        .page_active = &p->poll_gate,
        .poll_once = is_moon ? moon_poll_once : net_poll_once,
        .interval_ms = interval_cb,
        .backoff_initial_ms = is_moon ? 0 : 15000,
        .backoff_max_ms = is_moon ? 0 : 300000,
        .on_park = park_cb,
    };
    poll_loop_run(&spec, p);
}
