# Config Backup & Restore — Design Spec

**Date:** 2026-03-14
**Config Version:** 22
**Status:** Draft

## Overview

Add config backup (export) and restore (import) functionality to the web UI. Users can download their device configuration as a JSON file and restore it later — on the same device or a different one. The feature handles version mismatches gracefully using JSON field overlay (not binary migration), with a two-step preview-then-confirm restore flow.

## Backup File Format

Plain JSON file with three top-level sections:

```json
{
  "meta": {
    "config_version": 22,
    "firmware_version": "v2.1.0",
    "git_sha": "15b5a7f",
    "hostname": "NINA-DISPLAY",
    "mac_address": "A1:B2:C3:D4:E5:F6",
    "export_date": "2026-03-14T20:30:00Z"
  },
  "config": {
    "theme_index": 5,
    "brightness": 75,
    "update_rate_s": 2
  },
  "sensitive": {
    "mqtt_username": "user",
    "mqtt_password": "pass123",
    "mqtt_broker_url": "mqtt://broker.lan",
    "spotify_client_id": "abc123",
    "hostname": "NINA-DISPLAY"
  }
}
```

- **`meta`** — always present. Device identity + version info for display in restore preview.
- **`config`** — always present. All non-sensitive settings.
- **`sensitive`** — only present when user opts in during export. Contains MQTT credentials, Spotify client ID, and hostname. If absent on import, those fields are left untouched.

### Filename Convention

```
{hostname}_{MAC}_{configVersion}_{date}.json
```

Example: `NINA-DISPLAY_A1B2C3D4E5F6_v22_2026-03-14.json`

MAC address has colons stripped for filesystem compatibility. The `Content-Disposition` header sets this filename on download.

## Sensitive Fields

| Field | Reason |
|-------|--------|
| `mqtt_username` | Network credential |
| `mqtt_password` | Network credential |
| `mqtt_broker_url` | May contain embedded auth |
| `spotify_client_id` | OAuth app identifier |
| `hostname` | Device-specific identity |

WiFi credentials and Spotify OAuth tokens are stored in separate NVS namespaces and are **not included** in the backup at all.

## API Endpoints

### `GET /api/config/backup?include_sensitive=0|1`

Downloads the backup JSON file.

- Query parameter `include_sensitive` (default `0`): when `1`, includes the `sensitive` section.
- Response `Content-Type: application/json`
- Response `Content-Disposition: attachment; filename="NINA-DISPLAY_A1B2C3D4E5F6_v22_2026-03-14.json"`
- Builds metadata from `BUILD_GIT_TAG`, `BUILD_GIT_SHA`, device MAC via `esp_read_mac()`, current hostname, and ISO-8601 UTC timestamp.
- Config fields are serialized using the same JSON key names as the existing `GET /api/config` endpoint.

### `POST /api/config/restore`

Handles both preview and apply in a single endpoint.

**Request body:**

```json
{
  "backup": { ...the full backup JSON object... },
  "confirm": false
}
```

- `confirm: false` → **preview mode**: parse, validate, diff against current config, return preview response.
- `confirm: true` → **apply mode**: overlay backup fields onto current config, validate, save to NVS, return success.

**Preview response (`confirm: false`):**

```json
{
  "status": "preview",
  "version_match": "older",
  "backup_version": 20,
  "current_version": 22,
  "backup_firmware": "v2.0.0",
  "backup_hostname": "NINA-DISPLAY",
  "backup_mac": "A1:B2:C3:D4:E5:F6",
  "export_date": "2026-03-14T20:30:00Z",
  "warnings": [
    "Backup is from an older config version (v20 → v22). Settings added since v20 will keep their current values."
  ],
  "missing_fields": ["spotify_overlay_timeout_s", "spotify_minimal_mode"],
  "unknown_fields": [],
  "validation_notes": [],
  "sensitive_included": false,
  "sensitive_excluded": ["mqtt_password", "mqtt_username", "mqtt_broker_url", "spotify_client_id", "hostname"],
  "changes": {
    "Display": [
      {"field": "theme_index", "label": "Theme", "from": 0, "to": 5},
      {"field": "brightness", "label": "Brightness", "from": 50, "to": 75}
    ],
    "Behavior": [
      {"field": "auto_rotate_enabled", "label": "Auto-rotate", "from": false, "to": true}
    ]
  },
  "no_changes": ["Nodes & Data", "System", "AllSky", "Spotify", "MQTT"],
  "total_changes": 3
}
```

**Apply response (`confirm: true`):**

```json
{
  "status": "applied",
  "total_changes": 3,
  "validation_notes": []
}
```

**Error responses:**

```json
{"error": "Invalid backup file: missing 'meta' section"}
{"error": "Invalid backup file: no config_version in metadata"}
{"error": "Invalid backup file: JSON parse error"}
```

## Version Mismatch Handling

The restore uses **JSON field overlay**, not binary struct migration. This means:

1. Start with the current in-memory config (all fields at current values).
2. For each field in the backup's `config` (and optionally `sensitive`) section, if the field name is recognized by the current firmware, overlay it.
3. Unknown fields (from newer backups) are silently ignored but listed in `unknown_fields`.
4. Missing fields (from older backups) keep their current values and are listed in `missing_fields`.
5. Run `validate_config()` to clamp any out-of-range values; report clamped fields in `validation_notes`.

### Warning Logic

| Scenario | `version_match` | Warning Color | Message |
|----------|-----------------|---------------|---------|
| Same version | `"exact"` | Green | "Config version matches (v22). All settings will be restored." |
| Older backup (≤10 versions behind) | `"older"` | Amber | "Backup is from an older config version (v{X} → v{Y}). Settings added since v{X} will keep their current values." |
| Much older backup (>10 versions behind) | `"much_older"` | Red/Amber | "Backup is from a much older version (v{X} → v{Y}). Only basic settings will be restored. Most settings will keep current values." |
| Newer backup | `"newer"` | Amber | "Backup is from a newer firmware (v{X} → v{Y}). Some settings may not be recognized by this firmware and will be skipped." |
| Invalid/missing version | — | Red | "Invalid backup file." (rejected) |

## Field Category Mapping

Fields grouped by the existing config UI tab structure for the diff display:

- **Display:** `theme_index`, `brightness`, `color_brightness`, `widget_style`, `screen_rotation`
- **Behavior:** `auto_rotate_enabled`, `auto_rotate_interval_s`, `auto_rotate_effect`, `auto_rotate_skip_disconnected`, `auto_rotate_pages`, `update_rate_s`, `idle_poll_interval_s`, `connection_timeout_s`, `toast_duration_s`, `graph_update_interval_s`, `active_page_override`, `alert_flash_enabled`, `screen_sleep_enabled`, `screen_sleep_timeout_s`
- **Nodes & Data:** `api_url` (url1/url2/url3), `instance_enabled` (1/2/3), `filter_colors` (1/2/3), `rms_thresholds` (1/2/3), `hfr_thresholds` (1/2/3)
- **System:** `ntp_server` (as `ntp`), `tz_string` (as `timezone`), `debug_mode`, `demo_mode`, `auto_update_check`, `update_channel`, `deep_sleep_enabled`, `deep_sleep_wake_timer_s`, `deep_sleep_on_idle`, `wifi_power_save`
- **AllSky:** `allsky_enabled`, `allsky_hostname`, `allsky_update_interval_s`, `allsky_dew_offset`, `allsky_field_config`, `allsky_thresholds`
- **Spotify:** `spotify_enabled`, `spotify_poll_interval_ms`, `spotify_show_progress_bar`, `spotify_overlay_timeout_s`, `spotify_minimal_mode`
- **MQTT:** `mqtt_enabled`, `mqtt_port`, `mqtt_topic_prefix`

## Web UI — "Backup" Tab

New tab in `config_ui.html`, positioned after Spotify (rightmost tab).

### Export Section

- Current config version and firmware version displayed as info text.
- Checkbox: "Include sensitive data (MQTT credentials, Spotify client ID, hostname)" — unchecked by default.
- "Download Backup" button — triggers `GET /api/config/backup?include_sensitive=0|1` and browser-downloads the file.

### Import Section

- File input accepting `.json` files.
- On file selection, JavaScript reads the file locally, sends `POST /api/config/restore` with `confirm: false`.
- **Preview panel** appears with:
  - **Header:** Backup metadata — source hostname, MAC, firmware version, export date.
  - **Version banner:** Color-coded (green/amber/red) per warning logic above. Shows current and backup config versions.
  - **Changes diff:** Grouped by category. Each category is a collapsible section showing `field label: old → new`. Categories with no changes collapsed to a "No changes" line.
  - **Missing fields notice** (if older backup): Lists field names not present in the backup.
  - **Unknown fields notice** (if newer backup): Lists field names not recognized by current firmware.
  - **Validation notes** (if any values were clamped): Lists adjustments.
  - **Sensitive fields notice:** If sensitive section was absent, lists which fields will be untouched.
- "Cancel" button — dismisses the preview.
- "Restore" button — sends `POST /api/config/restore` with `confirm: true`. On success, shows toast and refreshes page.

## Implementation Files

### New/Modified Files

| File | Change |
|------|--------|
| `main/web_handlers_config.c` | Add `backup_get_handler()` and `restore_post_handler()` functions |
| `main/web_server.c` | Register two new routes: `GET /api/config/backup`, `POST /api/config/restore` |
| `main/web_server_internal.h` | Declare new handler functions |
| `main/config_ui.html` | Add "Backup" tab with export/import UI, preview panel, diff display |

### No New Files Required

All functionality fits within existing files. The backup/restore handlers follow the same patterns as `config_get_handler` and `config_post_handler`.

## Memory Considerations

- Backup JSON is built with `cJSON` and sent in one response (config is ~6.8KB struct → ~4-8KB JSON).
- Restore POST body can be up to ~16KB (backup JSON + `confirm` wrapper). The existing `receive_json_body()` uses an 8KB SPIRAM buffer — this needs to be increased to 16KB for the restore endpoint, or the backup JSON can be received as a multipart upload.
- All temporary allocations use SPIRAM (`MALLOC_CAP_SPIRAM`).
- The diff computation is done server-side to keep the HTML/JS lightweight.

## Edge Cases

1. **Empty file upload** → JSON parse fails → "Invalid backup file" error.
2. **Valid JSON, wrong structure** (no `meta`/`config` keys) → "Invalid backup format" error.
3. **Backup from same device** → hostname/MAC match shown in preview (user can confirm it's theirs).
4. **Backup from different device** → hostname/MAC differ — preview shows source device info for context.
5. **User exports with sensitive, shares file** → sensitive fields clearly labeled in export UI so user knows what's in the file.
6. **Restore then regret** → config is saved to NVS on confirm. User can factory reset but cannot auto-revert. Consider: should we auto-backup before restore? (Decision: no — keep it simple, user can manually export first.)
7. **Large JSON string fields** (allsky_field_config, allsky_thresholds) → diff shows "Modified" with truncated preview rather than full JSON blob.
8. **Concurrent access** → config mutex protects read/write; restore is serialized with other config operations.
9. **Browser refresh during preview** → preview is client-side state only, no server state to clean up.
10. **POST body too large** → increase `receive_json_body()` buffer to 16KB for restore endpoint.
