/* Host test for main/app_config_forward.h -- pure decision function for
 * forward-tolerant config load (config_accept_forward()). Header-only,
 * no ESP/LVGL dependency; assert-style like test/host/test_route_auth.c.
 *
 * Fleet-safety-critical: a firmware downgrade must not wipe a newer config
 * blob back to factory defaults. This truth table pins the exact accept/
 * reject boundary described in main/app_config_forward.h.
 */
#include "app_config_forward.h"
#include <stdio.h>

static int fails = 0;

static void check_bool(const char *label, bool got, bool expect) {
    printf("%-70s got=%-5s expect=%-5s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

int main(void) {
    const uint32_t FW = 49;
    const size_t FW_SIZE = 7580; /* representative sizeof(app_config_t) */

    /* -- accept: newer version, blob at least as large as firmware -------- */
    check_bool("accept: v50, size > fw_size",
               config_accept_forward(50, FW_SIZE + 128, FW, FW_SIZE), true);
    check_bool("accept: v51, size much bigger",
               config_accept_forward(51, FW_SIZE + 4096, FW, FW_SIZE), true);
    check_bool("accept: boundary blob_size == fw_size",
               config_accept_forward(50, FW_SIZE, FW, FW_SIZE), true);

    /* -- reject: same version (handled by exact-match branch, not this) --- */
    check_bool("reject: same version, larger size",
               config_accept_forward(FW, FW_SIZE + 128, FW, FW_SIZE), false);
    check_bool("reject: same version, exact size",
               config_accept_forward(FW, FW_SIZE, FW, FW_SIZE), false);

    /* -- reject: older version (handled by migration ladder, not this) ---- */
    check_bool("reject: older version v48",
               config_accept_forward(48, FW_SIZE + 128, FW, FW_SIZE), false);
    check_bool("reject: v0",
               config_accept_forward(0, FW_SIZE + 128, FW, FW_SIZE), false);

    /* -- reject: legacy/garbage blob (huge first word, v0 ASCII SSID) ------ */
    check_bool("reject: version >= 0x1000 (legacy ASCII SSID pattern)",
               config_accept_forward(0x1000, FW_SIZE + 128, FW, FW_SIZE), false);
    check_bool("reject: version way above ceiling (printable ASCII garbage)",
               config_accept_forward(0x41414141u, FW_SIZE + 128, FW, FW_SIZE), false);

    /* -- reject: newer version but truncated/corrupt/foreign blob --------- */
    check_bool("reject: newer version, blob_size < fw_size",
               config_accept_forward(50, FW_SIZE - 1, FW, FW_SIZE), false);
    check_bool("reject: newer version, tiny blob_size",
               config_accept_forward(50, 4, FW, FW_SIZE), false);

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
