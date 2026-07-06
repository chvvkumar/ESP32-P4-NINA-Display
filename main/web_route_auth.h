#pragma once

/**
 * @file web_route_auth.h
 * @brief Pure decision function for the default-deny web route auth gate.
 *
 * No ESP-IDF includes on purpose: this header (and its .c) must be
 * host-testable with nothing beyond stdbool/stddef. All ESP-specific
 * plumbing (session cookie check, setup-mode check, sending the 401/302)
 * lives in main/web_server.c, which calls route_auth_allows() with the
 * already-resolved booleans.
 */

#include <stdbool.h>

/**
 * Auth classification for a registered route. Enum value 0 is the
 * fail-closed default: a route_entry_t that omits .auth in an aggregate
 * initializer gets ROUTE_AUTH_REQUIRED, not accidental public access.
 */
typedef enum {
    ROUTE_AUTH_REQUIRED = 0, /* default: session or X-Auth-Password required */
    ROUTE_PUBLIC,            /* always reachable, no auth check performed */
    ROUTE_SETUP_EXEMPT,      /* reachable during first-run setup mode, or with a valid session */
} route_auth_class_t;

/**
 * @brief Decide whether a request to a classified route is allowed through.
 *
 * Truth table:
 *   ROUTE_PUBLIC       -> always true
 *   ROUTE_SETUP_EXEMPT -> setup_mode || authed
 *   ROUTE_AUTH_REQUIRED (or any unrecognized value) -> authed
 *
 * @param cls        route classification
 * @param setup_mode true when the device is in first-run WiFi setup mode
 * @param authed     true when the caller already has a valid session/header auth
 * @return true if the request should be dispatched to its handler
 */
bool route_auth_allows(route_auth_class_t cls, bool setup_mode, bool authed);
