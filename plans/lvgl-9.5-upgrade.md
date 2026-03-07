# LVGL 9.2.0 -> 9.5.0 Upgrade Plan

**Current:** LVGL 9.2.0 | **Target:** LVGL 9.5.0
**Risk Level:** Low-Medium
**Branch:** Create `lvgl-9.5-upgrade` from `main`

---

## Phase 1: Dependency Updates

- [x] **Update version pin** in `main/idf_component.yml` line 6:
  ```yaml
  # FROM:
  lvgl/lvgl:
    version: "9.2.0"
  # TO:
  lvgl/lvgl:
    version: "~9.5.0"
  ```

- [ ] **Delete cached managed component** to force re-fetch:
  ```bash
  rm -rf managed_components/lvgl__lvgl
  ```

- [ ] **Fetch new dependencies:**
  ```bash
  idf.py update-dependencies
  ```

### Upstream Constraints (verified compatible, no action needed)

| Component | Version | LVGL Constraint | Compatible? |
|-----------|---------|----------------|-------------|
| `espressif/esp_lvgl_port` | 2.7.2 | `>=8,<10` | Yes |
| `waveshare/esp32_p4_wifi6_touch_lcd_4b` | 1.0.1 | `>=8,<10` | Yes |

Both auto-detect LVGL 9.x at build time via CMake version checks. No BSP or port update required.

---

## Phase 2: Breaking Change Remediation

### 2.1 `lvgl_private.h` Separation

Starting in 9.2.0 and enforced more strictly in 9.3+, internal LVGL APIs were separated into `lvgl_private.h`. Code that accesses internal struct fields or functions will fail to compile without it.

**Files most likely to need `#include "lvgl_private.h"`:**

- [x] `main/ui/ui_styles.c` (~line 15) — added `#include "lvgl_private.h"` after project includes
- [x] `main/ui/lv_font_material_safety.c` (~line 15) — added `#include "lvgl_private.h"` after `#include "lvgl.h"`
- [x] `main/ui/lv_font_superscript_24.c` (~line 12) — added `#include "lvgl_private.h"` after include guard `#endif`

**Alternative:** Instead of per-file includes, set `LV_USE_PRIVATE_API=y` in Kconfig (`idf.py menuconfig` -> LVGL -> Misc -> Enable private API). This auto-includes `lvgl_private.h` everywhere. Less surgical but faster.

### 2.2 Non-Applicable Breaking Changes (no action needed)

| Breaking Change | Why N/A |
|----------------|---------|
| XML engine removed (9.5.0) | Project does not use LVGL XML UI engine |
| Wayland CSD removed (9.5.0) | Embedded ESP32 project, not using Wayland |
| `lv_fragment` deprecated (9.5.0) | Project does not use `lv_fragment` |
| `LV_PROPERTY_TEXTAREA_INSERT_REPLACE` removed (9.5.0) | Project does not use this property |

---

## Phase 3: Build & Fix Cycle

- [ ] **Full clean build:**
  ```bash
  idf.py fullclean
  idf.py build
  ```

- [ ] **Fix any `lvgl_private.h` compilation errors** (see Phase 2.1 for expected files)

- [ ] **Fix any other compilation errors** — check for:
  - Undeclared struct fields in custom fonts -> add `#include "lvgl_private.h"`
  - Undeclared struct fields in draw callbacks -> add `#include "lvgl_private.h"`
  - Any renamed/moved macros (unlikely but check)

- [ ] **Address deprecation warnings** (if any appear):
  - `lv_fragment` warnings -> ignore, not used
  - Other warnings -> evaluate case by case

- [ ] **Verify clean build with zero errors**

---

## Phase 4: Kconfig / sdkconfig Verification

### Existing settings to verify still apply

| Setting | Current Value | 9.5 Status | Action |
|---------|--------------|------------|--------|
| `CONFIG_LV_USE_PPA=y` | Enabled | PPA draw unit for fills + images | Enabled with DPI framebuffers |
| `CONFIG_BSP_LCD_DPI_BUFFER_NUMS=2` | 2 framebuffers | Required for avoid_tearing | Set |
| `CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR=y` | Enabled | Uses DPI framebuffers | Set |
| `CONFIG_BSP_DISPLAY_LVGL_FULL_REFRESH=y` | Enabled | Full-screen rendering | Set |
| `CONFIG_LV_DRAW_BUF_STRIDE_ALIGN=1` | No padding | Must match PPA's pic_w stride | Keep at 1 |
| `CONFIG_LV_DRAW_BUF_ALIGN=128` | Cache-line aligned | Must equal CACHE_L2_CACHE_LINE_SIZE | Set |
| `CONFIG_LVGL_PORT_ENABLE_PPA=n` | Disabled | Only for SW rotation (not used) | No change |
| Dual draw units | Enabled | Still supported | No change |
| 1000 Hz FreeRTOS tick | Set | Unaffected | No change |

- [x] **Enable PPA** with DPI framebuffer mode (avoid_tearing + full_refresh)
- [ ] **Check for any new Kconfig defaults** that may have changed between 9.2 and 9.5
- [ ] **Verify `sdkconfig` diff** is minimal and expected after build

---

## Phase 5: On-Device Runtime Testing

Flash and verify all UI functionality:

### Core Navigation
- [ ] Swipe left/right through all pages — verify smooth transitions
- [ ] BOOT button page cycling — verify it skips settings page
- [ ] Auto-rotate — let it cycle through pages, verify fade/instant transitions
- [ ] Page indicator dots — verify correct count and highlighting

### Dashboard Widgets
- [ ] Arc widget — verify main dashboard arc renders with correct angles/rotation/knob
- [ ] Labels — verify all text renders (montserrat 12-48px range)
- [ ] Bars/sliders — verify progress bars and slider controls
- [ ] Buttons/checkboxes/switches — verify all interactive controls

### Custom Rendering
- [ ] Widget style mode 2 (Wireframe) — verify corner accent lines via custom draw callback
- [ ] Widget style mode 6 (Chamfered) — verify triangle decorations via custom draw callback
- [ ] Shadow/opacity — verify shadow rendering on summary cards

### Custom Fonts
- [ ] Safety icons (Material Symbols 40px) — verify `lv_font_material_safety` renders correctly
- [ ] Superscript numerals (24px) — verify `lv_font_superscript_24` on AllSky page

### Charts & Graphs
- [ ] Open RMS/HFR graph overlay — verify line rendering with 500-point history
- [ ] Chart threshold lines — verify horizontal reference lines
- [ ] Graph controls (scale, legend, point options) — verify functionality

### Layouts
- [ ] Bento-box grid layout on instance pages — verify spacing/alignment
- [ ] Flex layouts on AllSky quadrants — verify four-quadrant arrangement
- [ ] Settings 4-tab tabview — switch between all tabs
- [ ] Summary page glassmorphic cards — verify layout with all instances

### Overlays & Notifications
- [ ] Info overlays — tap each dashboard quadrant (camera, mount, sequence, filter, autofocus, imagestats, session)
- [ ] Thumbnail overlay — tap header to show JPEG image
- [ ] Toast notifications — trigger and verify stacking behavior
- [ ] Alert flash animations — verify screen-border flashing on threshold breach
- [ ] Event log overlay — open and verify scrollable history
- [ ] OTA dialog — verify renders (can test without actual update)

### System Features
- [ ] System info page — verify IP, WiFi, CPU, memory, PSRAM, uptime labels
- [ ] Deep sleep/wake — verify page index restoration after wake
- [ ] Screen rotation — test if rotation setting works correctly
- [ ] Screenshot endpoint — verify `/api/screenshot` still works (uses `lv_snapshot_take`)

### Settings Controls
- [ ] Display tab — brightness slider, color brightness slider, theme dropdown
- [ ] Behavior tab — auto-rotate switch, page skip checkboxes, deep sleep controls
- [ ] Nodes tab — textarea inputs for NINA URLs, keyboard interaction
- [ ] System tab — reboot button, factory reset, OTA check

---

## Phase 6: Optional Post-Upgrade Enhancements

These are NOT required for the upgrade but are newly available in 9.5:

| Feature | What It Does | Where to Use | Effort |
|---------|-------------|-------------|--------|
| `lv_indev_set_gesture_*_threshold()` | Fine-tune swipe sensitivity | `nina_dashboard.c` gesture handler | Low |
| `LV_CHART_TYPE_CURVE` | Smooth Bezier curves between data points | RMS/HFR graph overlay | Medium (needs `LV_USE_VECTOR_GRAPHICS`) |
| Native blur support | Software blur for frosted-glass effects | Summary page glassmorphic cards | Low |
| `LV_STATE_ALT` | Simple dark/light mode switching | Could simplify theme system | High (architectural) |
| `LV_OBJ_FLAG_RADIO_BUTTON` | Built-in radio button groups | Settings tabs with exclusive options | Low |
| `lv_slider_set_min_value/max_value` | Separate min/max setters for sliders | Settings sliders | Low |
| `lv_obj_set_scroll_x/y` properties | Direct scroll position setting | Event log, info overlays | Low |
| `lv_group_set_user_data()` | User data on input groups | Input management | Low |
| `lv_obj_set_user_data_free_cb()` | Auto-cleanup of user data | Any obj with malloc'd user_data | Low |

---

## API Compatibility Reference

### Verified Compatible (180+ calls, no changes needed)

All of the following APIs used in the project are unchanged between 9.2 and 9.5:

**Object lifecycle:** `lv_obj_create`, `lv_obj_delete`, `lv_obj_clean`, `lv_*_create` (all widget constructors)

**Style system:** All `lv_style_set_*` and `lv_obj_set_style_*` functions — bg_color, bg_opa, bg_grad_color, bg_grad_dir, border_width, border_color, border_opa, radius, pad_all, text_color, text_font, shadow_width, shadow_color, arc_color, arc_width, line_color, line_width, opa, outline_width, translate_x/y, margin_*, min_width, max_height, size

**Layout:** `lv_obj_set_flex_flow`, `lv_obj_set_flex_align`, `lv_obj_set_flex_grow`, `lv_obj_set_grid_dsc_array`, `lv_obj_set_grid_cell`, `LV_GRID_FR`, `LV_GRID_TEMPLATE_LAST`, `LV_LAYOUT_GRID`

**Events:** `lv_obj_add_event_cb`, `lv_event_get_target`, `lv_event_get_current_target`, `lv_event_get_code`, `lv_event_get_user_data`, `lv_event_get_layer` — all event type constants (`LV_EVENT_CLICKED`, `LV_EVENT_VALUE_CHANGED`, `LV_EVENT_GESTURE`, `LV_EVENT_DRAW_MAIN_END`, etc.)

**Animation:** `lv_anim_init`, `lv_anim_set_var`, `lv_anim_set_values`, `lv_anim_set_duration`, `lv_anim_set_delay`, `lv_anim_set_exec_cb`, `lv_anim_set_completed_cb`, `lv_anim_set_path_cb`, `lv_anim_start`, `lv_anim_delete`

**Drawing:** `lv_draw_line`, `lv_draw_line_dsc_init`, `lv_draw_triangle`, `lv_draw_triangle_dsc_init`, `lv_snapshot_take`, `lv_draw_buf_destroy`

**Display:** `lv_display_get_default`, `lv_display_set_rotation`, `lv_scr_act`, `lv_layer_top`

**Widgets:** All arc, chart, label, tabview, bar, slider, checkbox, dropdown, buttonmatrix, switch, line, textarea, keyboard, msgbox APIs — creation and property functions unchanged

**Gestures:** `lv_indev_get_act`, `lv_indev_get_gesture_dir`, `LV_DIR_LEFT`, `LV_DIR_RIGHT`

**Timers:** `lv_timer_create`, `lv_timer_delete`, `lv_timer_set_repeat_count`

**Colors:** `lv_color_hex`, `lv_color_black`, `lv_color_white`

**Constants:** All `LV_PART_*`, `LV_STATE_*`, `LV_ALIGN_*`, `LV_OPA_*`, `LV_OBJ_FLAG_*`, `LV_SIZE_CONTENT`, `LV_RADIUS_CIRCLE`, `LV_PCT()`, `LV_SYMBOL_*`, `LV_ANIM_ON/OFF` — unchanged

### Bug Fixes That May Improve Behavior (automatic, no code changes)

- Flex grow rounding — eliminated unused space on flex tracks
- Flex grow with `LV_SIZE_CONTENT` parent — min size now used correctly
- Arc indicator padding — knob position and invalidation area fixed
- Label drawing — skips rendering if width is negative
- Line drawing — checks minimum 2 points before drawing (prevents buffer overflow)

---

## Risk Matrix

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `lvgl_private.h` compile errors | **High** | Low | Add include to 2-3 files or set `LV_USE_PRIVATE_API=y` |
| Custom font struct incompatibility | Medium | Medium | Re-generate fonts with 9.5 font converter if struct layout changed |
| Flex/grid layout rendering changes | Low | Medium | Bug fixes improve behavior; visual verify on device |
| Draw callback API changes | Very Low | High | APIs are stable; verify at compile time |
| esp_lvgl_port incompatibility | Very Low | High | Port explicitly supports `>=8,<10` |
| Kconfig defaults changed | Low | Low | Diff sdkconfig after menuconfig |

---

## Files Modified (Expected)

| File | Change | Reason |
|------|--------|--------|
| `main/idf_component.yml` | Version bump `9.2.0` -> `~9.5.0` | Dependency update |
| `main/ui/ui_styles.c` | Add `#include "lvgl_private.h"` | Draw callback internals |
| `main/ui/lv_font_material_safety.c` | Add `#include "lvgl_private.h"` | Font format internals |
| `main/ui/lv_font_superscript_24.c` | Add `#include "lvgl_private.h"` | Font format internals |
| `sdkconfig` | Auto-updated by build | New Kconfig entries from 9.5 |
