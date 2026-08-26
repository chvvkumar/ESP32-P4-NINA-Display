#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Anonymous telemetry: one small JSON health report a day, POSTed to
 * https://ninadash.challa.co/v1/report, gated on cfg->telemetry_enabled.
 *
 * The payload is anonymous by construction: telemetry_build_payload() is the
 * single source of truth for every byte that leaves the device (the
 * /api/telemetry/preview endpoint returns the exact same bytes), and it
 * carries NO user-configured value: no SSID, URL, hostname, coordinate,
 * token or password. The device id is 16 random bytes minted once at first
 * boot (NVS "app_conf"/"dev_uuid"), unrelated to any hardware identifier.
 *
 * features.pages bitmask:
 *   bit 0 allsky_enabled     bit 1 spotify_enabled   bit 2 goes_enabled
 *   bit 3 moon_enabled       bit 4 solar_enabled     bit 5 custom_enabled
 *   bit 6 radar_enabled      bit 7 clouds_enabled    bit 8 json_enabled
 *   bit 9 ha_enabled         bit 10 octoprint_enabled
 *   bit 11 flights_enabled   bit 12 demo_mode        bit 13 auto_rotate_enabled
 * features.integrations bitmask:
 *   bit 0 mqtt_enabled
 *   bit 1 weather configured (location set, plus an API key when the
 *         provider needs one; same derivation as /api/status)
 *   bit 2 auth_enabled       bit 3 debug_mode
 *   bit 4 deep_sleep_enabled bit 5 audio_muted */

/* Load or mint the device UUID (8-4-4-4-12 lowercase hex). Idempotent; call
 * once from app_main() before the web server starts. */
void telemetry_init(void);

/* Render the exact report JSON into @p buf. Returns the length written
 * (excluding the NUL) or a negative value when @p buf cannot hold it.
 * @p include_crash gates the crash{} block (still subject to the last reset
 * actually being abnormal): the sender passes true only for the FIRST report
 * of a boot so one panic is counted once server-side, not once per daily
 * report; the preview endpoint always passes true. The crash block carries
 * reason, cumulative count, crashed task name, faulting PC and the panic
 * text (sanitized, truncated to 120 chars), all read from the coredump the
 * crash wrote; task/pc/detail are empty when no coredump is readable. */
int telemetry_build_payload(char *buf, size_t cap, bool include_crash);

/* Sole owner/installer of the ESP32-P4 SoC temperature sensor (range -10..80).
 * Returns the last known reading in Celsius, 0 until the first success. */
float telemetry_read_temp_c(void);

/* Daily report loop. Spawned only through telemetry_ensure_task_running()
 * (tasks.h); never blocks boot. */
void telemetry_task(void *arg);
