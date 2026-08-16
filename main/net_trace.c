/**
 * @file net_trace.c
 * @brief WiFi traffic debug facility. See net_trace.h for the contract.
 *
 * Event ring in PSRAM; a 250 ms esp_timer sampler reads the esp_hosted
 * transport packet counters (CONFIG_ESP_HOSTED_PKT_STATS) into a 30 s bin
 * window and flags TX bursts that no firmware source announced.
 * All timestamps are (uint32_t)(esp_timer_get_time() / 1000).
 */
#include "net_trace.h"

#include <string.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "lwip/stats.h"   /* LWIP_STATS from lwipopts.h via lwip/opt.h */

static const char *TAG = "net";

#define SRC_NAME_LEN 16
#define UNATTR_QUIET_MS 500   /* TX with no NET_EV_TX in this window = unattributed */
#define NET_UNATTR_MIN_PKTS 1 /* smaller unattributed bins stay in the sparkline only, no ring record */

#if CONFIG_ESP_HOSTED_PKT_STATS
/* Mirror of managed_components/espressif__esp_hosted/host/utils/stats.h
 * (struct pkt_stats_t, host/utils/stats.h:123). That header is a private
 * include of the component, so the layout is duplicated here. esp_hosted is
 * pinned >=2.12.3,<2.12.4 in main/idf_component.yml; re-check the field
 * order on any bump. */
struct pkt_stats_t {
    uint32_t sta_rx_in;
    uint32_t sta_rx_out;
    uint32_t sta_tx_in_pass;
    uint32_t sta_tx_trans_in;
    uint32_t sta_tx_flowctrl_drop;
    uint32_t sta_tx_out;
    uint32_t sta_tx_out_drop;
    uint32_t sta_flow_ctrl_on;
    uint32_t sta_flow_ctrl_off;
};
extern struct pkt_stats_t pkt_stats;
#endif

/* ---- state -------------------------------------------------------------- */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static net_trace_rec_t *s_ring;          /* NET_TRACE_RING records, PSRAM */
static uint32_t s_count;                 /* total records ever written */
static uint32_t s_last_tx_ms;            /* t_ms of the newest NET_EV_TX */
static bool s_verbose;

EXT_RAM_BSS_ATTR static char     s_src_name[NET_TRACE_MAX_SRC][SRC_NAME_LEN];
EXT_RAM_BSS_ATTR static uint32_t s_src_count[NET_TRACE_MAX_SRC];
static size_t s_src_n;

EXT_RAM_BSS_ATTR static net_sched_t s_sched[NET_TRACE_MAX_SCHED];
EXT_RAM_BSS_ATTR static char        s_sched_name[NET_TRACE_MAX_SCHED][SRC_NAME_LEN];
static size_t s_sched_n;

/* Bin window: circular, s_bin_head = next slot to write, s_bin_n = filled. */
EXT_RAM_BSS_ATTR static uint16_t s_bin_tx[NET_TRACE_BINS];
EXT_RAM_BSS_ATTR static uint16_t s_bin_rx[NET_TRACE_BINS];
EXT_RAM_BSS_ATTR static uint32_t s_bin_ts[NET_TRACE_BINS];   /* sample time closing the bin */
static size_t s_bin_head, s_bin_n;
static uint32_t s_prev_tx, s_prev_rx;

static esp_timer_handle_t s_timer;

#if LWIP_STATS
/* lwIP per-protocol xmit counters (CONFIG_LWIP_STATS). Names TX the
 * firmware sources never announce: ARP, ICMP, IGMP/MLD, ND6, and TCP
 * keepalives/ACKs. */
enum { P_TCP, P_UDP, P_ICMP, P_ARP, P_IGMP, P_IP6, P_ICMP6, P_MLD6, P_ND6, P_N };
static const char *const s_proto_name[P_N] = {
    "tcp", "udp", "icmp", "arp", "igmp", "ip6", "icmp6", "mld6", "nd6"
};
static uint32_t s_proto_prev[P_N];

/* lwIP per-protocol recv counters: what came IN. udp.recv already counts the
 * datagrams later dropped for no listener (mDNS/SSDP), so it is used alone. */
enum { R_LINK, R_ARP, R_IP, R_ICMP, R_IGMP, R_UDP, R_TCP, R_IP6, R_ICMP6, R_MLD6, R_ND6, R_N };
static const char *const s_rx_name[R_N] = {
    "link", "arp", "ip", "icmp", "igmp", "udp", "tcp", "ip6", "icmp6", "mld6", "nd6"
};
static uint32_t s_rx_prev[R_N];
#define RX_RING 12   /* last 3 s of per-tick RX deltas, written under s_lock */
EXT_RAM_BSS_ATTR static uint16_t s_rx_ring[RX_RING][R_N];
static size_t s_rx_head;
#endif

/* ---- helpers ------------------------------------------------------------ */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Caller holds s_lock. */
static void ring_push_locked(uint8_t kind, uint8_t src, uint16_t pkts, uint32_t t)
{
    if (!s_ring) return;
    net_trace_rec_t *r = &s_ring[s_count % NET_TRACE_RING];
    r->t_ms = t;
    r->kind = kind;
    r->src  = src;
    r->pkts = pkts;
    s_count++;
}

/* Caller holds s_lock. Returns 0xFF when the table is full. */
static uint8_t intern_locked(const char *name)
{
    for (size_t i = 0; i < s_src_n; i++) {
        if (strncmp(s_src_name[i], name, SRC_NAME_LEN - 1) == 0) return (uint8_t)i;
    }
    if (s_src_n >= NET_TRACE_MAX_SRC) return 0xFF;
    strncpy(s_src_name[s_src_n], name, SRC_NAME_LEN - 1);
    s_src_name[s_src_n][SRC_NAME_LEN - 1] = '\0';
    return (uint8_t)s_src_n++;
}

#if LWIP_STATS
static void proto_read(uint32_t out[P_N])
{
    memset(out, 0, P_N * sizeof(uint32_t));
#if TCP_STATS
    out[P_TCP] = lwip_stats.tcp.xmit;
#endif
#if UDP_STATS
    out[P_UDP] = lwip_stats.udp.xmit;
#endif
#if ICMP_STATS
    out[P_ICMP] = lwip_stats.icmp.xmit;
#endif
#if ETHARP_STATS
    out[P_ARP] = lwip_stats.etharp.xmit;
#endif
#if IGMP_STATS
    out[P_IGMP] = lwip_stats.igmp.xmit;
#endif
#if IP6_STATS
    out[P_IP6] = lwip_stats.ip6.xmit;
#endif
#if ICMP6_STATS
    out[P_ICMP6] = lwip_stats.icmp6.xmit;
#endif
#if MLD6_STATS
    out[P_MLD6] = lwip_stats.mld6.xmit;
#endif
#if ND6_STATS
    out[P_ND6] = lwip_stats.nd6.xmit;
#endif
}

static void rx_read(uint32_t out[R_N])
{
    memset(out, 0, R_N * sizeof(uint32_t));
#if LINK_STATS
    out[R_LINK] = lwip_stats.link.recv;
#endif
#if ETHARP_STATS
    out[R_ARP] = lwip_stats.etharp.recv;
#endif
#if IP_STATS
    out[R_IP] = lwip_stats.ip.recv;
#endif
#if ICMP_STATS
    out[R_ICMP] = lwip_stats.icmp.recv;
#endif
#if IGMP_STATS
    out[R_IGMP] = lwip_stats.igmp.recv;
#endif
#if UDP_STATS
    out[R_UDP] = lwip_stats.udp.recv;
#endif
#if TCP_STATS
    out[R_TCP] = lwip_stats.tcp.recv;
#endif
#if IP6_STATS
    out[R_IP6] = lwip_stats.ip6.recv;
#endif
#if ICMP6_STATS
    out[R_ICMP6] = lwip_stats.icmp6.recv;
#endif
#if MLD6_STATS
    out[R_MLD6] = lwip_stats.mld6.recv;
#endif
#if ND6_STATS
    out[R_ND6] = lwip_stats.nd6.recv;
#endif
}

/* Per-tick delta of a counter table, wrap-safe at the STAT_COUNTER width
 * (u16 unless LWIP_STATS_LARGE). Updates prev in place. */
static void stat_delta(const uint32_t *now, uint32_t *prev, uint32_t *d, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t v = now[i] - prev[i];
        if (sizeof(STAT_COUNTER) == 2) v &= 0xFFFF;
        d[i] = v;
        prev[i] = now[i];
    }
}

static void proto_delta(uint32_t d[P_N])
{
    uint32_t now[P_N];
    proto_read(now);
    stat_delta(now, s_proto_prev, d, P_N);
}

static void rx_delta(uint32_t d[R_N])
{
    uint32_t now[R_N];
    rx_read(now);
    stat_delta(now, s_rx_prev, d, R_N);
}

/* "tcp 3 arp 1" for the non-zero entries; "" when all zero. Stops when full. */
static void stat_fmt(const char *const *name, const uint32_t *d, int n, char *out, size_t len)
{
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (!d[i]) continue;
        int w = snprintf(out + used, len - used, "%s%s %u", used ? " " : "", name[i], (unsigned)d[i]);
        if (w < 0 || (size_t)w >= len - used) break;
        used += (size_t)w;
    }
}
#endif /* LWIP_STATS */

static void sampler_cb(void *arg)
{
    (void)arg;
    uint32_t tx = 0, rx = 0;
#if LWIP_STATS
    uint32_t dp[P_N], dr[R_N];
    proto_delta(dp);
    rx_delta(dr);
#endif
#if CONFIG_ESP_HOSTED_PKT_STATS
    tx = pkt_stats.sta_tx_out;
    rx = pkt_stats.sta_rx_in;
#endif
    uint32_t dtx = tx - s_prev_tx;
    uint32_t drx = rx - s_prev_rx;
    s_prev_tx = tx;
    s_prev_rx = rx;
    uint16_t dtx16 = dtx > 0xFFFF ? 0xFFFF : (uint16_t)dtx;
    uint16_t drx16 = drx > 0xFFFF ? 0xFFFF : (uint16_t)drx;
    uint32_t t = now_ms();
    bool unattr = false;

    portENTER_CRITICAL(&s_lock);
    s_bin_tx[s_bin_head] = dtx16;
    s_bin_rx[s_bin_head] = drx16;
    s_bin_ts[s_bin_head] = t;
    s_bin_head = (s_bin_head + 1) % NET_TRACE_BINS;
    if (s_bin_n < NET_TRACE_BINS) s_bin_n++;
#if LWIP_STATS
    for (int i = 0; i < R_N; i++) s_rx_ring[s_rx_head][i] = dr[i] > 0xFFFF ? 0xFFFF : (uint16_t)dr[i];
    s_rx_head = (s_rx_head + 1) % RX_RING;
#endif
    if (dtx >= NET_UNATTR_MIN_PKTS && (uint32_t)(t - s_last_tx_ms) > UNATTR_QUIET_MS) {
        ring_push_locked(NET_EV_UNATTR, 0xFF, dtx16, t);
        unattr = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!s_verbose) return;
#if LWIP_STATS
    char pb[96];
    stat_fmt(s_proto_name, dp, P_N, pb, sizeof(pb));
    if (unattr) {
        /* per-tick RX breakdown only on bins big enough to be worth reading */
        char rb[96 + 5] = "";
        if (dtx >= 3) {
            char rt[96];
            stat_fmt(s_rx_name, dr, R_N, rt, sizeof(rt));
            snprintf(rb, sizeof(rb), " rx[%s]", rt);
        }
        ESP_LOGI(TAG, "TX ?? %u pkt (rx %u) [%s]%s", (unsigned)dtx, (unsigned)drx, pb, rb);
    } else if (dp[P_ARP] | dp[P_ICMP] | dp[P_IGMP] | dp[P_ICMP6] | dp[P_MLD6] | dp[P_ND6]) {
        /* stack chatter no firmware source announces, even inside an attributed bin */
        ESP_LOGI(TAG, "TX proto %s", pb);
    }
#else
    if (unattr) ESP_LOGI(TAG, "TX ?? %u pkt (rx %u)", (unsigned)dtx, (unsigned)drx);
#endif
}

/* ---- API ---------------------------------------------------------------- */

void net_trace_init(void)
{
    if (s_ring) return;
    s_ring = heap_caps_calloc(NET_TRACE_RING, sizeof(net_trace_rec_t), MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        ESP_LOGE(TAG, "ring alloc failed");
        return;
    }
    s_last_tx_ms = now_ms();
#if CONFIG_ESP_HOSTED_PKT_STATS
    s_prev_tx = pkt_stats.sta_tx_out;
    s_prev_rx = pkt_stats.sta_rx_in;
#endif
#if LWIP_STATS
    proto_read(s_proto_prev);
    rx_read(s_rx_prev);
#endif
    const esp_timer_create_args_t args = {
        .callback = sampler_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "net_trace",
    };
    if (esp_timer_create(&args, &s_timer) == ESP_OK) {
        esp_timer_start_periodic(s_timer, (uint64_t)NET_TRACE_BIN_MS * 1000);
    }
    ESP_LOGI(TAG, "net_trace ready (pkt_stats %s)", net_trace_available() ? "on" : "off");
}

void net_ev_note_detail(const char *src, const char *detail)
{
    if (!src) src = "?";
    uint32_t t = now_ms();
    portENTER_CRITICAL(&s_lock);
    uint8_t idx = intern_locked(src);
    if (idx != 0xFF) s_src_count[idx]++;
    ring_push_locked(NET_EV_TX, idx, 0, t);
    s_last_tx_ms = t;
    portEXIT_CRITICAL(&s_lock);
    if (s_verbose) {
        if (detail) ESP_LOGI(TAG, "TX %s %s", src, detail);
        else        ESP_LOGI(TAG, "TX %s", src);
    }
}

void net_ev_note(const char *src)
{
    net_ev_note_detail(src, NULL);
}

void net_sched_note(const char *name, uint32_t due_ms)
{
    if (!name) return;
    portENTER_CRITICAL(&s_lock);
    size_t i;
    for (i = 0; i < s_sched_n; i++) {
        if (strncmp(s_sched_name[i], name, SRC_NAME_LEN - 1) == 0) break;
    }
    if (i == s_sched_n) {
        if (s_sched_n >= NET_TRACE_MAX_SCHED) {
            portEXIT_CRITICAL(&s_lock);
            static bool warned;
            if (!warned) {
                warned = true;
                ESP_LOGW(TAG, "sched table full (%d), dropping '%s'", NET_TRACE_MAX_SCHED, name);
            }
            return;
        }
        strncpy(s_sched_name[i], name, SRC_NAME_LEN - 1);
        s_sched_name[i][SRC_NAME_LEN - 1] = '\0';
        s_sched[i].name = s_sched_name[i];
        s_sched[i].due_ms = due_ms;
        s_sched[i].period_ms = 0;
        s_sched[i].count = 0;
        s_sched_n++;
    } else {
        /* due_ms == 0 is the parked sentinel: no period across a park edge */
        s_sched[i].period_ms = (due_ms && s_sched[i].due_ms) ? due_ms - s_sched[i].due_ms : 0;
        s_sched[i].due_ms = due_ms;
    }
    s_sched[i].count++;
    portEXIT_CRITICAL(&s_lock);
}

void net_trace_mark(void)
{
    uint32_t t = now_ms();
    portENTER_CRITICAL(&s_lock);
    ring_push_locked(NET_EV_MARK, 0xFF, 0, t);
    portEXIT_CRITICAL(&s_lock);
    /* Last 3 s of transport bins, oldest first, so a mark with no host TX
     * still shows what came IN (unicast RX = C6-side ACK TX). */
    uint16_t btx[12], brx[12];
    net_trace_bins(btx, brx, 12);
    char bins[12 * 12 + 1];
    size_t off = 0;
    for (int i = 0; i < 12 && off < sizeof(bins) - 12; i++) {
        off += (size_t)snprintf(bins + off, sizeof(bins) - off, "%u/%u ", (unsigned)btx[i], (unsigned)brx[i]);
    }
    ESP_LOGI(TAG, "NET MARK [tx/rx x250ms: %s]", bins);
#if LWIP_STATS
    /* Same 3 s, classified by lwIP recv counters (order-free sum over the ring). */
    uint32_t rs[R_N] = {0};
    portENTER_CRITICAL(&s_lock);
    for (int k = 0; k < RX_RING; k++) {
        for (int i = 0; i < R_N; i++) rs[i] += s_rx_ring[k][i];
    }
    portEXIT_CRITICAL(&s_lock);
    char rb[96];
    stat_fmt(s_rx_name, rs, R_N, rb, sizeof(rb));
    ESP_LOGI(TAG, "NET MARK rx3s [%s]", rb);
#endif
}

void net_trace_set_verbose(bool on) { s_verbose = on; }
bool net_trace_verbose(void)        { return s_verbose; }
uint32_t net_trace_count(void)      { return s_count; }

bool net_trace_read(uint32_t back, net_trace_rec_t *out)
{
    if (!out || !s_ring) return false;
    bool ok = false;
    portENTER_CRITICAL(&s_lock);
    uint32_t avail = s_count < NET_TRACE_RING ? s_count : NET_TRACE_RING;
    if (back < avail) {
        *out = s_ring[(s_count - 1 - back) % NET_TRACE_RING];
        ok = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

size_t net_trace_src_n(void) { return s_src_n; }

const char *net_trace_src_name(uint8_t idx)
{
    return idx < s_src_n ? s_src_name[idx] : "";
}

uint32_t net_trace_src_count(uint8_t idx)
{
    return idx < s_src_n ? s_src_count[idx] : 0;
}

size_t net_sched_list(net_sched_t *out, size_t max)
{
    if (!out || max == 0) return 0;
    portENTER_CRITICAL(&s_lock);
    size_t n = s_sched_n < max ? s_sched_n : max;
    memcpy(out, s_sched, n * sizeof(net_sched_t));
    portEXIT_CRITICAL(&s_lock);
    return n;
}

void net_trace_bins(uint16_t *tx_out, uint16_t *rx_out, size_t n)
{
    if (n > NET_TRACE_BINS) n = NET_TRACE_BINS;
    if (n == 0) return;
    portENTER_CRITICAL(&s_lock);
    size_t have = s_bin_n < n ? s_bin_n : n;
    size_t pad = n - have;
    /* oldest of the last `have` bins */
    size_t idx = (s_bin_head + NET_TRACE_BINS - have) % NET_TRACE_BINS;
    for (size_t i = 0; i < n; i++) {
        uint16_t tx = 0, rx = 0;
        if (i >= pad) {
            tx = s_bin_tx[idx];
            rx = s_bin_rx[idx];
            idx = (idx + 1) % NET_TRACE_BINS;
        }
        if (tx_out) tx_out[i] = tx;
        if (rx_out) rx_out[i] = rx;
    }
    portEXIT_CRITICAL(&s_lock);
}

void net_trace_totals(uint32_t *tx, uint32_t *rx, uint32_t *tx_drop, uint32_t *flowctrl_on)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
#if CONFIG_ESP_HOSTED_PKT_STATS
    a = pkt_stats.sta_tx_out;
    b = pkt_stats.sta_rx_in;
    c = pkt_stats.sta_tx_out_drop;
    d = pkt_stats.sta_flow_ctrl_on;
#endif
    if (tx) *tx = a;
    if (rx) *rx = b;
    if (tx_drop) *tx_drop = c;
    if (flowctrl_on) *flowctrl_on = d;
}

void net_trace_peak(uint16_t *pkts, char *srcs, size_t srcs_len)
{
    uint16_t best = 0;
    uint32_t best_ts = 0;
    uint32_t mask = 0;   /* bit i = source i had a TX event in the peak bin */
    _Static_assert(NET_TRACE_MAX_SRC <= 32, "peak mask is 32-bit");

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < s_bin_n; i++) {
        /* newest first so ties resolve to the most recent bin */
        size_t idx = (s_bin_head + NET_TRACE_BINS - 1 - i) % NET_TRACE_BINS;
        if (s_bin_tx[idx] > best) {
            best = s_bin_tx[idx];
            best_ts = s_bin_ts[idx];
        }
    }
    if (best > 0 && s_ring) {
        uint32_t avail = s_count < NET_TRACE_RING ? s_count : NET_TRACE_RING;
        for (uint32_t k = 0; k < avail; k++) {
            const net_trace_rec_t *r = &s_ring[(s_count - 1 - k) % NET_TRACE_RING];
            if (r->kind != NET_EV_TX || r->src >= NET_TRACE_MAX_SRC) continue;
            /* bin covers (ts - BIN_MS, ts]; the check is a wrap-safe delta */
            uint32_t age = best_ts - r->t_ms;
            if (age <= NET_TRACE_BIN_MS) mask |= 1u << r->src;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (pkts) *pkts = best;
    if (srcs && srcs_len) {
        srcs[0] = '\0';
        size_t used = 0;
        for (uint8_t i = 0; i < NET_TRACE_MAX_SRC && i < s_src_n; i++) {
            if (!(mask & (1u << i))) continue;
            int w = snprintf(srcs + used, srcs_len - used, "%s%s", used ? "," : "", s_src_name[i]);
            if (w < 0 || (size_t)w >= srcs_len - used) break;
            used += (size_t)w;
        }
    }
}

bool net_trace_available(void)
{
#if CONFIG_ESP_HOSTED_PKT_STATS
    return true;
#else
    return false;
#endif
}
