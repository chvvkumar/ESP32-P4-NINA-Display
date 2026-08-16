/**
 * @file net_trace.h
 * @brief WiFi traffic debug facility: attributes C6 transport TX bursts to the
 * firmware source that triggered them (teal-flash correlation aid).
 *
 * Contract shared with the LVGL overlay UI. Add only; never remove or rename.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NET_TRACE_RING     256   // events retained
#define NET_TRACE_MAX_SRC  24    // distinct interned source names
#define NET_TRACE_MAX_SCHED 16   // countdown slots
#define NET_TRACE_BINS     120   // 250 ms bins = 30 s window
#define NET_TRACE_BIN_MS   250

typedef enum { NET_EV_TX = 0, NET_EV_MARK = 1, NET_EV_UNATTR = 2 } net_ev_kind_t;

typedef struct {
    uint32_t t_ms;   // esp_timer_get_time()/1000, low 32 bits
    uint8_t  kind;   // net_ev_kind_t
    uint8_t  src;    // interned source index for NET_EV_TX, 0xFF otherwise
    uint16_t pkts;   // NET_EV_UNATTR: transport TX pkts in that bin; else 0
} net_trace_rec_t;   // 8 bytes

typedef struct {
    const char *name;    // interned, never freed
    uint32_t due_ms;     // absolute ms when the source intends to fire next; 0 = parked (page-gated poller idle, no next fire)
    uint32_t period_ms;  // due_ms delta between the last two notes (0 = unknown)
    uint32_t count;      // notes so far
} net_sched_t;

void net_trace_init(void);                                    // app_main, after perf_monitor_init(); allocates ring in PSRAM, starts 250 ms esp_timer sampler
void net_ev_note(const char *src);                            // any task, cheap; interns src (strncmp over <=16 names), appends NET_EV_TX, bumps per-src count; ESP_LOGI line only when verbose
void net_ev_note_detail(const char *src, const char *detail);  // same as net_ev_note; verbose line reads "TX <src> <detail>" (detail NULL = net_ev_note)
void net_sched_note(const char *name, uint32_t due_ms);       // any task; before sleeping; updates/creates slot, computes period_ms; due_ms=0 marks the slot parked
void net_trace_mark(void);                                    // appends NET_EV_MARK; ALWAYS ESP_LOGI "NET MARK"
void net_trace_set_verbose(bool on);                          // per-event log lines on/off (UI toggles while overlay open)
bool net_trace_verbose(void);
uint32_t net_trace_count(void);                               // total records ever written
bool     net_trace_read(uint32_t back, net_trace_rec_t *out); // back=0 newest, back<min(count,RING); false past end
size_t   net_trace_src_n(void);
const char *net_trace_src_name(uint8_t idx);                  // "" if out of range
uint32_t net_trace_src_count(uint8_t idx);
size_t   net_sched_list(net_sched_t *out, size_t max);        // snapshot copy, returns n
void     net_trace_bins(uint16_t *tx_out, uint16_t *rx_out, size_t n); // last n bins (n<=NET_TRACE_BINS), oldest first, zero-filled if fewer
void     net_trace_totals(uint32_t *tx, uint32_t *rx, uint32_t *tx_drop, uint32_t *flowctrl_on); // transport cumulative; all zero if PKT_STATS off
void     net_trace_peak(uint16_t *pkts, char *srcs, size_t srcs_len); // worst TX bin in the 30 s window; srcs = comma list of distinct source names whose TX events fell in that bin (may be "")
bool     net_trace_available(void);                           // true when CONFIG_ESP_HOSTED_PKT_STATS transport counters exist
