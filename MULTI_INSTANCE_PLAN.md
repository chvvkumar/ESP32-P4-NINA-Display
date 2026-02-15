# Multi-Instance NINA Dashboard Implementation Plan

Support monitoring up to 3 NINA instances with per-instance pages, lazy data fetching,
and web UI configuration. Each page uses the identical dashboard layout — only the bound
data changes between pages.

---

## Current State Summary

- **Polling**: `main.c` already creates `d1`/`d2` with `nina_poll_state_t` each; both are
  polled every 2 seconds unconditionally
- **WebSocket**: Only instance 1 has a WebSocket connection (hardcoded single `ws_client`)
- **Dashboard**: Single `create_nina_dashboard()` builds one set of LVGL widgets;
  `update_nina_dashboard_ui()` only reads `data1`
- **Config**: `app_config_t` stores `api_url_1` and `api_url_2` (no third URL)
- **Web UI**: Two URL input fields, no per-instance filter/threshold settings
- **Input**: Boot button handler exists but does nothing

---

## Phase 1 — NVS / Config Changes

### 1.1 Extend `app_config_t` for 3 instances
- [ ] Add `api_url_3[128]` field to `app_config_t`
- [ ] Add `instance_count` derived helper or just count non-empty URLs at runtime
- [ ] Add per-instance `filter_colors` (currently shared globally) — decide:
  keep global for simplicity since filters are typically the same across rigs,
  or duplicate per instance. **Recommendation: keep global filter colors/brightness
  for v1**, only the API URLs differ per instance.
- [ ] Add `active_page` (int, 0-2) to persist which page was last viewed (optional,
  can default to 0 on boot)
- [ ] Ensure `app_config_save()` / `app_config_init()` handle new fields with safe
  defaults (empty string for `api_url_3`, preserving backward compat with existing
  NVS blobs)

### 1.2 Filter sync per instance
- [ ] Currently `filters_synced` is a single bool and only syncs instance 1 filters.
  Change to `bool filters_synced[3]` so each instance can independently sync its
  filter list to NVS on first connect. Since filter colors are global, merging
  filter names from all instances into one color map is fine.

---

## Phase 2 — NINA Client Changes

### 2.1 Third instance data
- [ ] Add `d3` / `poll_state3` in `data_update_task()` alongside existing `d1`, `d2`

### 2.2 Per-instance WebSocket support
- [ ] Refactor `nina_websocket_start()` / `nina_websocket_stop()` to support multiple
  concurrent WebSocket connections. Currently uses a single static `ws_client` handle.
  Options:
  - **A) One WS at a time** (active page only) — simpler, saves RAM, aligns with
    "don't fetch when not on screen" goal
  - **B) All WS connections always open** — more responsive page switching, higher
    resource use
- [ ] **Recommendation: Option A** — only maintain a WebSocket for the currently
  displayed instance. On page switch, stop the old WS and start the new one.
- [ ] Add `nina_websocket_start_for(int instance_index, const char *url, nina_client_t *data)`
  or change the API to accept an instance identifier so events route to the correct
  `nina_client_t`

### 2.3 Conditional polling (CPU/API load reduction)
- [ ] Expose `active_page` as a shared variable (or callback) accessible from
  `data_update_task()`
- [ ] **Active page instance**: Full tiered polling (ALWAYS + FAST + CONDITIONAL + SLOW)
  as today
- [ ] **Background instances**: Reduce to heartbeat-only polling — just
  `fetch_camera_info_robust()` every ~10 seconds to maintain connection status.
  Skip guider RMS, image history, sequence, focuser, mount, switch. This keeps
  the "connected" indicator accurate without burning API calls.
- [ ] When switching pages, immediately do a full poll for the newly active instance
  so data appears without waiting for the next cycle
- [ ] Stop/start WebSocket on page switch per 2.2 above

---

## Phase 3 — Dashboard UI Changes (LVGL)

### 3.1 Multi-page container architecture
- [ ] Create an array of 3 page containers (`lv_obj_t *pages[3]`), each a full-screen
  child of the screen object
- [ ] Only **one page is visible** at a time (`lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN)`
  for inactive pages)
- [ ] Each page is built with the **exact same** `create_nina_dashboard_page()` function
  — identical grid, identical widget structure, identical styling. Only the data
  pointer differs.
- [ ] Refactor `create_nina_dashboard()`:
  ```
  for (int i = 0; i < configured_instance_count; i++) {
      pages[i] = create_nina_dashboard_page(parent);
  }
  show_page(0);  // default to first
  ```

### 3.2 Per-page widget references
- [ ] Currently all widget pointers (labels, arcs, containers) are file-scope statics.
  Refactor into a `dashboard_page_t` struct that holds all widget references for one page:
  ```c
  typedef struct {
      lv_obj_t *page;           // root container
      lv_obj_t *telescope_lbl;
      lv_obj_t *target_lbl;
      lv_obj_t *arc;
      lv_obj_t *rms_lbl;
      lv_obj_t *hfr_lbl;
      // ... all other widget refs
  } dashboard_page_t;
  ```
- [ ] Allocate `dashboard_page_t pages[3]` — one per possible instance
- [ ] All update functions take a `dashboard_page_t *` instead of using globals

### 3.3 Update function changes
- [ ] Rename / refactor `update_nina_dashboard_ui()` to accept a page index or operate
  on the active page only:
  ```c
  void update_nina_dashboard_page(dashboard_page_t *page, const nina_client_t *data);
  ```
- [ ] In the main loop, only call update for the **active page**:
  ```c
  update_nina_dashboard_page(&pages[active_page], &instances[active_page]);
  ```
  This naturally avoids updating hidden pages, saving CPU.

### 3.4 Page indicator
- [ ] Add a small page indicator at the bottom or top of the screen (e.g., 3 dots,
  active dot highlighted) so the user knows which page they're on and how many
  are available
- [ ] Only show the indicator when `configured_instance_count > 1`
- [ ] The indicator should be a shared overlay, not per-page, to avoid redundancy

### 3.5 Theme application
- [ ] `nina_dashboard_apply_theme()` must iterate all created pages and apply the theme
  to each, so switching themes updates all pages (even hidden ones), and they look
  correct when switching to them

---

## Phase 4 — Page Navigation

### 4.1 Hardware button navigation (GPIO 35 — built-in BOOT button)
- [ ] The ESP32-P4 board has a built-in button on `GPIO_NUM_35` (active-low, pull-up
  enabled). An `input_task` already exists in `main.c` that polls this GPIO at 100ms
  intervals and detects press edges — currently a no-op stub.
- [ ] Add debounce logic (ignore presses within 200ms of the last accepted press)
- [ ] On valid press, cycle `active_page`:
  `active_page = (active_page + 1) % configured_instance_count`
- [ ] Communicate the page change to `data_update_task` via a shared `volatile int`
  or a FreeRTOS event/notification so the polling task can react immediately
- [ ] On page change:
  1. Hide current page container, show new page container
  2. Stop WebSocket for old instance, start for new instance
  3. Trigger immediate full poll for newly active instance (don't wait for next 2s cycle)
  4. Update page indicator dots
- [ ] If only 1 instance is configured, button press does nothing (no page to switch to)

### 4.2 Touch/swipe navigation (optional enhancement)
- [ ] Add left/right swipe gesture detection on the LVGL screen to switch pages
- [ ] Use `lv_obj_add_event_cb()` with `LV_EVENT_GESTURE` and check
  `lv_indev_get_gesture_dir()`
- [ ] Same page-switch logic as button

### 4.3 API-driven page switch (optional)
- [ ] Add `POST /api/page` endpoint to web server allowing remote page switching
- [ ] Useful for automation or remote control scenarios

---

## Phase 5 — Web UI Changes

### 5.1 Third NINA URL field
- [ ] Add "NINA Instance 3 URL" input field in the HTML config page
- [ ] Apply the same auto-formatting logic as instances 1 and 2 (hostname → full URL)

### 5.2 Instance labels (optional)
- [ ] Add optional "Instance Name" text fields (e.g., "Refractor", "Newt", "Wide Field")
  for display on the page indicator or header
- [ ] Store in `app_config_t` as `instance_name_1[32]`, etc.
- [ ] If blank, default to "Instance 1", "Instance 2", "Instance 3"

### 5.3 Config API changes
- [ ] `GET /api/config` returns `api_url_3` (and instance names if added)
- [ ] `POST /api/config` accepts and saves the new fields

---

## Phase 6 — Testing & Validation

### 6.1 Single instance (regression)
- [ ] Only `api_url_1` configured → behavior identical to current: one page, no indicator,
  full polling, WebSocket active
- [ ] No visible UI changes from before

### 6.2 Two instances
- [ ] Both URLs configured → 2 pages, page indicator shows 2 dots
- [ ] Button press cycles between pages
- [ ] Inactive page stops receiving full polls (heartbeat only)
- [ ] WebSocket switches on page change
- [ ] Filter sync works for both instances

### 6.3 Three instances
- [ ] All three URLs configured → 3 pages, 3 dots
- [ ] Button cycles 1→2→3→1
- [ ] Memory usage acceptable (3 full dashboard widget trees + 3 `nina_client_t`)

### 6.4 Load/performance
- [ ] Verify CPU usage drops when background instances are heartbeat-only
- [ ] Verify no HTTP timeouts or WebSocket reconnect storms on page switch
- [ ] Measure LVGL RAM usage with 3 pages — ensure no heap exhaustion

### 6.5 Web UI
- [ ] All 3 URL fields work, save, and restore correctly
- [ ] Factory reset clears all 3 URLs
- [ ] Existing 2-URL configs migrate cleanly (api_url_3 defaults to empty)

---

## Implementation Order

| Step | Phase | Description | Risk |
|------|-------|-------------|------|
| 1 | 1.1 | Add `api_url_3` to config, update save/load | Low |
| 2 | 5.1 | Add third URL field to web UI | Low |
| 3 | 3.2 | Create `dashboard_page_t` struct, refactor widget refs | Medium |
| 4 | 3.1 | Multi-page container with hidden/visible toggling | Medium |
| 5 | 3.3 | Refactor update function to per-page | Medium |
| 6 | 2.1 | Add d3/poll_state3 in main | Low |
| 7 | 2.3 | Conditional polling (active vs background) | Medium |
| 8 | 2.2 | Per-instance WebSocket (stop/start on switch) | High |
| 9 | 4.1 | Boot button page cycling | Low |
| 10 | 3.4 | Page indicator dots | Low |
| 11 | 3.5 | Theme applies to all pages | Low |
| 12 | 1.2 | Per-instance filter sync | Low |
| 13 | 6.x | Testing & validation | — |
| 14 | 4.2 | Touch/swipe navigation (optional) | Medium |
| 15 | 5.2 | Instance names (optional) | Low |

---

## Key Design Decisions

1. **Pages vs. re-binding a single page**: Using separate LVGL widget trees per page
   is cleaner than swapping data pointers on a single widget set. It avoids flicker,
   preserves animation state (arc progress), and lets LVGL's hidden-flag optimization
   skip rendering of off-screen pages.

2. **Global filter colors**: Keeping one set of filter colors shared across all instances
   simplifies config and UI. Most multi-rig setups use the same filter naming.

3. **Active-only WebSocket**: Running one WS connection at a time saves ~20KB RAM per
   connection and reduces event processing load. The tradeoff is a ~1s delay on page
   switch while the new WS connects — acceptable for a monitoring display.

4. **Heartbeat-only background polling**: Background instances only check
   `fetch_camera_info_robust()` on a slower interval. This keeps the connection status
   dot accurate while dropping API load by ~90% for inactive instances.

5. **Backward compatibility**: Existing NVS configs with 2 URLs continue to work.
   `api_url_3` defaults to empty. Single-instance setups see zero UI changes.
