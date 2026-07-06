/* Host test for main/web_route_auth.c -- pure decision function for the
 * default-deny web route auth gate (route_auth_allows()). No ESP/LVGL
 * dependency; assert-style like test/host/test_graph_downsample.c.
 *
 * Also documents (in the block comment below) the route classification
 * snapshot expected in main/web_server.c's routes[] table, so a reviewer
 * changing one can be reminded to check the other. */
#include "web_route_auth.h"
#include <stdio.h>

/*
 * Expected allowlist snapshot (must match main/web_server.c routes[]):
 *
 *   ROUTE_PUBLIC (no auth check at all, ever):
 *     /favicon.ico, /login, /api/login, /api/auth/status,
 *     /api/version, /api/spotify/callback
 *
 *   ROUTE_SETUP_EXEMPT (reachable during first-run setup, else needs auth):
 *     / (root GET), /api/wifi/scan, /api/wifi/setup, /api/wifi/status
 *
 *   ROUTE_AUTH_REQUIRED (default -- everything else, ~59 routes)
 */

static int fails = 0;

static void check_bool(const char *label, bool got, bool expect) {
    printf("%-55s got=%-5s expect=%-5s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

int main(void) {
    /* -- ROUTE_PUBLIC: always true, regardless of setup_mode/authed -------- */
    check_bool("PUBLIC, setup=false, authed=false",
               route_auth_allows(ROUTE_PUBLIC, false, false), true);
    check_bool("PUBLIC, setup=false, authed=true",
               route_auth_allows(ROUTE_PUBLIC, false, true), true);
    check_bool("PUBLIC, setup=true,  authed=false",
               route_auth_allows(ROUTE_PUBLIC, true, false), true);
    check_bool("PUBLIC, setup=true,  authed=true",
               route_auth_allows(ROUTE_PUBLIC, true, true), true);

    /* -- ROUTE_SETUP_EXEMPT: setup_mode || authed --------------------------- */
    check_bool("SETUP_EXEMPT, setup=false, authed=false",
               route_auth_allows(ROUTE_SETUP_EXEMPT, false, false), false);
    check_bool("SETUP_EXEMPT, setup=false, authed=true",
               route_auth_allows(ROUTE_SETUP_EXEMPT, false, true), true);
    check_bool("SETUP_EXEMPT, setup=true,  authed=false",
               route_auth_allows(ROUTE_SETUP_EXEMPT, true, false), true);
    check_bool("SETUP_EXEMPT, setup=true,  authed=true",
               route_auth_allows(ROUTE_SETUP_EXEMPT, true, true), true);

    /* -- ROUTE_AUTH_REQUIRED: authed only ------------------------------------ */
    check_bool("AUTH_REQUIRED, setup=false, authed=false",
               route_auth_allows(ROUTE_AUTH_REQUIRED, false, false), false);
    check_bool("AUTH_REQUIRED, setup=false, authed=true",
               route_auth_allows(ROUTE_AUTH_REQUIRED, false, true), true);
    check_bool("AUTH_REQUIRED, setup=true,  authed=false",
               route_auth_allows(ROUTE_AUTH_REQUIRED, true, false), false);
    check_bool("AUTH_REQUIRED, setup=true,  authed=true",
               route_auth_allows(ROUTE_AUTH_REQUIRED, true, true), true);

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
