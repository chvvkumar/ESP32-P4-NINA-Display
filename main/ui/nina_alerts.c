/**
 * @file nina_alerts.c
 * @brief Screen-border flash animation for critical events (thread-safe).
 *
 * Architecture: nina_alert_trigger() pushes to a spinlock-protected
 * queue and returns immediately.  An LVGL timer (100 ms) pops one entry
 * per idle tick and fires the flash animation in LVGL context, so
 * simultaneous breaches each get their own full flash instead of the
 * last writer stomping the others.
 *
 * nina_alert_eval() sits on top: per (type, instance) breach state that
 * drives the flash path plus the voice announcements in audio_alert.c.
 */

#include "nina_alerts.h"
#include "nina_dashboard_internal.h"
#include "themes.h"
#include "display_defs.h"
#include "app_config.h"
#include "audio_alert.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

static const char *TAG = "alerts";

/* alert_type_t doubles as the bit index into alert_voice_types, so appending a
 * type only needs a matching ALERT_VOICE_TYPE_* define -- no code change here. */
_Static_assert(ALERT_VOICE_TYPE_RMS    == (1u << ALERT_RMS)  &&
               ALERT_VOICE_TYPE_HFR    == (1u << ALERT_HFR)  &&
               ALERT_VOICE_TYPE_SAFETY == (1u << ALERT_SAFETY),
               "alert_voice_types bits must match alert_type_t order");

#define ALERT_COOLDOWN_MS   30000   /* 30 s between same-type alerts */
#define FLASH_BORDER_W      8
#define FLASH_STEP_MS       150     /* Duration of each color step */
#define FLASH_CYCLES        3       /* red-white-red = 3 steps per cycle, 3 cycles */
#define ALERT_QUEUE_DEPTH   8       /* Pending flashes; deeper than 3 instances x 3 types can realistically breach at once */
#define ALERT_HYST_FACTOR   0.95f   /* Recovery threshold = 0.95 * ok_max */

/* ── Pending trigger entry ──────────────────────────────────────────── */
typedef struct {
    alert_type_t type;
    int          instance;
    float        value;
} pending_alert_t;

/* ── Per (type, instance) breach state for the eval engine ──────────── */
typedef struct {
    bool    breached;
    int64_t last_announce_ms;
    float   last_announce_value;  /* Repeat suppressor: an unchanged sample is not new data */
} breach_state_t;

/* ── Module state ───────────────────────────────────────────────────── */
static lv_obj_t        *s_flash_overlay = NULL;
static lv_timer_t      *s_alert_timer = NULL;
static int64_t          s_last_flash_ms[ALERT_TYPE_COUNT] = {0};  /* Per alert_type cooldown */
static pending_alert_t  s_queue[ALERT_QUEUE_DEPTH] = {0};
static int              s_q_head = 0;   /* Next slot to pop */
static int              s_q_count = 0;  /* Entries currently queued */
static breach_state_t   s_breach[ALERT_TYPE_COUNT][MAX_NINA_INSTANCES] = {0};
static portMUX_TYPE     s_alert_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── Flash animation state ──────────────────────────────────────────── */
static lv_timer_t *s_flash_anim_timer = NULL;
static int  s_flash_step = 0;       /* Current step in flash sequence */
static int  s_flash_total = 0;      /* Total steps (FLASH_CYCLES * 2) */

/* ── Helpers ────────────────────────────────────────────────────────── */
static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

/* Animation step callback for border color alternation */
static void flash_step_cb(lv_timer_t *timer) {
    if (!s_flash_overlay) {
        lv_timer_delete(timer);
        s_flash_anim_timer = NULL;
        return;
    }

    if (s_flash_step >= s_flash_total) {
        /* Animation complete — hide overlay */
        lv_obj_add_flag(s_flash_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_timer_delete(timer);
        s_flash_anim_timer = NULL;
        return;
    }

    /* Alternate red / white (or bright/dim red in Red Night) */
    uint32_t color;
    if (theme_is_red_night(current_theme)) {
        color = (s_flash_step % 2 == 0) ? 0xff0000 : 0x7f1d1d;
    } else {
        color = (s_flash_step % 2 == 0) ? 0xFF0000 : 0xFFFFFF;
    }
    lv_obj_set_style_border_color(s_flash_overlay, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(s_flash_overlay, 200, 0);
    s_flash_step++;
}

/* Fire the flash animation (LVGL context only) */
static void do_flash(void) {
    if (!s_flash_overlay) return;

    lv_obj_clear_flag(s_flash_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_flash_overlay);

    /* Start border color alternation: FLASH_CYCLES * 2 steps (red,white per cycle) */
    s_flash_step = 0;
    s_flash_total = FLASH_CYCLES * 2;

    /* Set initial red border */
    lv_obj_set_style_border_color(s_flash_overlay, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_border_opa(s_flash_overlay, 200, 0);
    s_flash_step = 1;

    /* Cancel any in-progress flash animation timer before starting a new one */
    if (s_flash_anim_timer) {
        lv_timer_delete(s_flash_anim_timer);
        s_flash_anim_timer = NULL;
    }
    s_flash_anim_timer = lv_timer_create(flash_step_cb, FLASH_STEP_MS, NULL);

    ESP_LOGI(TAG, "Alert flash triggered");
}

/* ── LVGL timer callback: drain the pending alert queue ─────────────── */
static void alert_tick_cb(lv_timer_t *timer) {
    (void)timer;

    /* One flash at a time: do_flash() cancels any in-progress animation, so
     * popping while one is running would collapse queued alerts into a single
     * flash.  Wait for the current sequence to finish instead. */
    if (s_flash_anim_timer) return;

    pending_alert_t local;
    bool have = false;

    portENTER_CRITICAL(&s_alert_lock);
    if (s_q_count > 0) {
        local = s_queue[s_q_head];
        s_q_head = (s_q_head + 1) % ALERT_QUEUE_DEPTH;
        s_q_count--;
        have = true;
    }
    portEXIT_CRITICAL(&s_alert_lock);

    if (have) {
        ESP_LOGD(TAG, "Flashing: type=%d instance=%d value=%.2f",
                 local.type, local.instance, local.value);
        do_flash();
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

void nina_alerts_init(lv_obj_t *screen) {
    if (!screen) return;

    /* Full-screen transparent overlay with thick border */
    s_flash_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_flash_overlay);
    lv_obj_set_size(s_flash_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_opa(s_flash_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_flash_overlay, FLASH_BORDER_W, 0);
    lv_obj_set_style_border_color(s_flash_overlay, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_border_opa(s_flash_overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_flash_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_flash_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_flash_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_flash_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Clear state */
    portENTER_CRITICAL(&s_alert_lock);
    memset(s_last_flash_ms, 0, sizeof(s_last_flash_ms));
    memset(s_queue, 0, sizeof(s_queue));
    memset(s_breach, 0, sizeof(s_breach));
    s_q_head = 0;
    s_q_count = 0;
    portEXIT_CRITICAL(&s_alert_lock);

    /* Drain timer: 100 ms interval */
    s_alert_timer = lv_timer_create(alert_tick_cb, 100, NULL);

    ESP_LOGI(TAG, "Alert system initialized");
}

void nina_alert_trigger(alert_type_t type, int instance, float value) {
    if (type < 0 || type >= ALERT_TYPE_COUNT) return;

    /* Cooldown read-modify-write of the 64-bit timestamp and the queue push
     * must be atomic together, else two tasks can both pass the cooldown
     * gate or tear the 64-bit timestamp. esp_timer_get_time() is safe in a
     * critical section. */
    int64_t t = now_ms();
    bool dropped = false;
    portENTER_CRITICAL(&s_alert_lock);
    if (t - s_last_flash_ms[type] < ALERT_COOLDOWN_MS) {
        portEXIT_CRITICAL(&s_alert_lock);
        return;
    }
    s_last_flash_ms[type] = t;

    if (s_q_count < ALERT_QUEUE_DEPTH) {
        int tail = (s_q_head + s_q_count) % ALERT_QUEUE_DEPTH;
        s_queue[tail].type = type;
        s_queue[tail].instance = instance;
        s_queue[tail].value = value;
        s_q_count++;
    } else {
        dropped = true;  /* Queue full — the flash is cosmetic, drop the newest */
    }
    portEXIT_CRITICAL(&s_alert_lock);

    ESP_LOGI(TAG, "Alert queued: type=%d instance=%d value=%.2f%s",
             type, instance, value, dropped ? " (DROPPED, queue full)" : "");
}

/* ── Breach engine ──────────────────────────────────────────────────── */

/**
 * Shared edge/hysteresis/repeat core.  @p edge is 1 = breach, 0 = clear,
 * -1 = hold (inside the hysteresis band: keep whatever state we are in).
 *
 * @p sampled marks a measured value (RMS, HFR) as opposed to a state
 * (safety).  A measurement only counts as new information when it actually
 * changed: instances[].hfr keeps its last value forever once the sequence
 * stops -- nothing clears it -- so without this the repeat branch would
 * re-speak one stale breaching frame every alert_voice_repeat_min for as
 * long as the instance stays connected.  A sustained unsafe state is not a
 * sample and must keep repeating, hence the flag rather than a blanket rule.
 *
 * The spinlock covers only the state update; audio_alert_speak() enqueues
 * onto its own FreeRTOS queue and must not be called from a critical section.
 */
static void alert_eval_edge(alert_type_t type, int instance, float value, int edge, bool sampled) {
    if (type < 0 || type >= ALERT_TYPE_COUNT) return;
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;

    app_config_t *cfg = app_config_get();
    /* 0 = never re-announce; only the entry edge speaks. */
    int64_t repeat_ms = (int64_t)cfg->alert_voice_repeat_min * 60000;

    int64_t t = now_ms();
    bool announce = false;
    bool breached;

    portENTER_CRITICAL(&s_alert_lock);
    breach_state_t *st = &s_breach[type][instance];
    if (edge == 0) {
        st->breached = false;               /* Recovered — re-arm the entry edge */
    } else if (edge == 1 && !st->breached) {
        st->breached = true;                /* Entry edge */
        st->last_announce_ms = t;
        st->last_announce_value = value;
        announce = true;
    } else if (st->breached && repeat_ms > 0 && t - st->last_announce_ms >= repeat_ms
               && (!sampled || value != st->last_announce_value)) {
        st->last_announce_ms = t;           /* Still breached — periodic re-announce */
        st->last_announce_value = value;
        announce = true;
    }
    breached = st->breached;
    portEXIT_CRITICAL(&s_alert_lock);

    /* Flash is attempted on every breached sample, exactly as before this
     * engine existed — nina_alert_trigger()'s 30 s per-type cooldown is what
     * actually paces it. */
    if (breached && cfg->alert_flash_enabled) {
        nina_alert_trigger(type, instance, value);
    }

    /* Voice is edge/repeat driven and gated independently of the flash. */
    if (announce && cfg->alert_voice_enabled && (cfg->alert_voice_types & (1u << type))) {
        audio_alert_speak(type, instance, value);
    }
}

void nina_alert_eval(alert_type_t type, int instance, float value, float threshold) {
    /* No threshold configured, or no sample this cycle — hold state silently. */
    if (threshold <= 0.0f || value <= 0.0f) return;

    int edge = -1;
    if (value > threshold)                            edge = 1;
    else if (value < threshold * ALERT_HYST_FACTOR)   edge = 0;
    /* else: inside the hysteresis band — hold */

    alert_eval_edge(type, instance, value, edge, true);
}

void nina_alert_eval_safety(int instance, bool is_safe) {
    alert_eval_edge(ALERT_SAFETY, instance, 0.0f, is_safe ? 0 : 1, false);
}

void nina_alerts_apply_theme(void) {
    /* Flash colors are set dynamically per step — no persistent state to restyle */
}
