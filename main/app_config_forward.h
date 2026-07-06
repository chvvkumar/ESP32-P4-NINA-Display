#pragma once

/*
 * Forward-tolerant config load — firmware downgrade case.
 *
 * app_config_t fields are append-only by protocol (CLAUDE.md "Configuration
 * Persistence": never reorder/insert, always append + bump APP_CONFIG_VERSION).
 * That means a config blob written by a NEWER firmware is always a strict
 * superset of the CURRENT (older, post-downgrade) firmware's app_config_t
 * layout: every field this firmware knows about sits at the same offset it
 * always has, and the newer blob simply has more bytes tacked on the end.
 *
 * So when a downgraded device boots into a config blob whose version is
 * newer than APP_CONFIG_VERSION, the safe move is to read the known prefix
 * (memcpy sizeof(app_config_t) bytes from the front of the blob) rather than
 * falling through to the terminal "unknown blob" branch, which wipes NVS
 * back to factory defaults and would silently reset an entire fleet's
 * settings on every downgrade.
 *
 * The 0x1000 ceiling on blob_version mirrors the existing legacy-v0 guard
 * (app_config.c, version_check > APP_CONFIG_VERSION && version_check < 0x1000):
 * a v0 blob's first four bytes are the wifi_ssid text, so version_check
 * there is either 0 (empty SSID) or a large printable-ASCII value. Any small
 * integer just above APP_CONFIG_VERSION is a plausible real future version
 * number, not a v0 blob or other garbage — that's the case this function
 * accepts.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Returns true iff the stored blob should be accepted as a forward
 * (newer-than-firmware) config and read via its known prefix:
 *   - blob_version is strictly newer than fw_version (otherwise this isn't
 *     the downgrade case — exact match and older-version migrations are
 *     handled by other branches).
 *   - blob_version is below the legacy-blob heuristic ceiling (rules out
 *     v0/garbage blobs whose first uint32_t happens to be a huge ASCII
 *     value).
 *   - blob_size is at least fw_size, so the known prefix is fully present
 *     (a genuine newer blob is a superset; anything smaller is corrupt or
 *     foreign data that merely happens to share a version-like first word).
 */
static inline bool config_accept_forward(uint32_t blob_version, size_t blob_size,
                                          uint32_t fw_version, size_t fw_size) {
    return blob_version > fw_version &&
           blob_version < 0x1000 &&
           blob_size >= fw_size;
}
