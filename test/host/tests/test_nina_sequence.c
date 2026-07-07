/* Host test for main/nina_sequence.c — the NINA sequence/json tree walker.
 *
 * Exercises fetch_sequence_counts_optional() against hand-built JSON
 * fixtures matching the LIVE ninaAPI v2 sequence/json shape (NOT the
 * saved-sequence-file shape): target name lives on a container whose
 * "Name" carries a "_Container" suffix that gets stripped; the active
 * target is the RUNNING child of "Targets_Container"; exposure counts are
 * only pulled from a descendant with Name=="Smart Exposure" AND
 * Status=="RUNNING" (see test_take_many_exposures_not_matched below for
 * the current-behavior gap this leaves for other step types).
 *
 * fetch_sequence_counts_optional() calls http_get_json() (declared in
 * nina_client_internal.h, implemented in nina_client.c which is NOT
 * linked here), parse_iso8601() (same header/source split), and
 * nina_client_now_epoch() (declared in nina_client.h). All three are
 * mocked below: http_get_json() parses whatever fixture string the test
 * pointed s_mock_json at; parse_iso8601() is a stub returning 0 since no
 * fixture here relies on the ExpectedDateTime fallback path;
 * nina_client_now_epoch() mirrors the production time(NULL) fallback.
 *
 * Build: see test/host/CMakeLists.txt (add_nina_host_test(test_nina_sequence ...)).
 */

#include "nina_sequence.h"
#include "nina_client_internal.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------
 * Mocks — nina_sequence.c references these two externs (declared in
 * nina_client_internal.h); real implementations live in nina_client.c,
 * which is deliberately not linked into this test binary.
 * --------------------------------------------------------------------- */
static const char *s_mock_json = NULL; /* NULL => simulate HTTP/parse failure */

cJSON *http_get_json(const char *url) {
    (void)url;
    if (!s_mock_json) {
        return NULL;
    }
    return cJSON_Parse(s_mock_json); /* NULL on malformed JSON, same as prod */
}

time_t parse_iso8601(const char *str) {
    (void)str;
    return 0; /* not exercised by these fixtures (all use RemainingTime) */
}

/* nina_client_now_epoch() (declared in nina_client.h, implemented in
 * nina_client.c) is referenced by the ExpectedDateTime fallback path in
 * find_earliest_condition(). Mirror the production fallback (device wall
 * clock); no fixture here exercises ExpectedDateTime, so the value is inert. */
int64_t nina_client_now_epoch(const nina_client_t *client) {
    (void)client;
    return (int64_t)time(NULL);
}

/* ---------------------------------------------------------------------
 * Assertion helpers (counter + PASS/FAIL prints, nonzero exit on failure —
 * same style as test/moon/test_moon_compute.c).
 * --------------------------------------------------------------------- */
static int fails = 0;

static void expect_str(const char *label, const char *got, const char *want) {
    int ok = (strcmp(got, want) == 0);
    printf("%-48s got=\"%s\" want=\"%s\" %s\n", label, got, want, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static void expect_int(const char *label, int got, int want) {
    int ok = (got == want);
    printf("%-48s got=%d want=%d %s\n", label, got, want, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static void expect_float(const char *label, float got, float want, float tol) {
    int ok = fabsf(got - want) <= tol;
    printf("%-48s got=%.2f want=%.2f %s\n", label, got, want, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static void reset_client(nina_client_t *c) {
    memset(c, 0, sizeof(*c));
}

static void run_fetch(const char *json, nina_client_t *c) {
    s_mock_json = json;
    fetch_sequence_counts_optional("http://fake-host/v2/api/", c);
    s_mock_json = NULL;
}

/* =======================================================================
 * (a) Normal running Smart Exposure with iterations, plus earliest-
 *     condition selection across two Conditions entries (also exercises
 *     classify_condition()'s "Horizon" -> "SETS IN" branch).
 * ======================================================================= */
static void test_normal_running_smart_exposure(void) {
    printf("\n-- test_normal_running_smart_exposure --\n");
    const char *json =
        "{"
        "  \"Response\": ["
        "    {"
        "      \"Name\": \"Targets_Container\","
        "      \"Items\": ["
        "        {"
        "          \"Name\": \"M31_Container\","
        "          \"Status\": \"RUNNING\","
        "          \"Conditions\": ["
        "            { \"Name\": \"Time Limit Condition\", \"RemainingTime\": \"5:00:00\" },"
        "            { \"Name\": \"Horizon Condition\", \"RemainingTime\": \"1:30:00\" }"
        "          ],"
        "          \"Items\": ["
        "            {"
        "              \"Name\": \"LRGBSHO_Container\","
        "              \"Status\": \"RUNNING\","
        "              \"Items\": ["
        "                {"
        "                  \"Name\": \"Smart Exposure\","
        "                  \"Status\": \"RUNNING\","
        "                  \"CompletedIterations\": 3,"
        "                  \"Iterations\": 10,"
        "                  \"ExposureCount\": 45,"
        "                  \"ExposureTime\": 300"
        "                }"
        "              ]"
        "            }"
        "          ]"
        "        }"
        "      ]"
        "    }"
        "  ]"
        "}";

    nina_client_t c;
    reset_client(&c);
    run_fetch(json, &c);

    expect_str("target_name (M31_Container stripped)", c.target_name, "M31");
    expect_str("prev_target_container (new RUNNING claim)", c.prev_target_container, "M31");
    expect_str("container_name (deepest RUNNING, stripped)", c.container_name, "LRGBSHO");
    expect_str("container_step (leaf under running container)", c.container_step, "Smart Exposure");
    expect_int("exposure_count (CompletedIterations)", c.exposure_count, 3);
    expect_int("exposure_iterations (Iterations)", c.exposure_iterations, 10);
    expect_int("exposure_total_count (ExposureCount)", c.exposure_total_count, 45);
    expect_float("exposure_total (ExposureTime)", c.exposure_total, 300.0f, 0.01f);
    expect_int("target_condition_count", c.target_condition_count, 2);
    /* Earliest of 5:00:00 (18000s) and 1:30:00 (5400s) is 1:30:00 -> "1h 30m",
     * and its Name ("Horizon Condition") classifies to "SETS IN". */
    expect_str("target_time_remaining (min of two conditions)", c.target_time_remaining, "1h 30m");
    expect_str("target_time_reason (Horizon -> SETS IN)", c.target_time_reason, "SETS IN");
}

/* =======================================================================
 * (b) No RUNNING target anywhere — only a FINISHED target container and a
 *     FINISHED Smart Exposure. Verifies the FINISHED fallback path fills
 *     target_name/container_name but does NOT touch prev_target_container,
 *     container_step, or the exposure counters (all stay at their
 *     zero-initialized defaults since nothing is RUNNING).
 * ======================================================================= */
static void test_no_running_target(void) {
    printf("\n-- test_no_running_target --\n");
    const char *json =
        "{"
        "  \"Response\": ["
        "    {"
        "      \"Name\": \"Targets_Container\","
        "      \"Items\": ["
        "        {"
        "          \"Name\": \"OldTarget_Container\","
        "          \"Status\": \"FINISHED\","
        "          \"Items\": ["
        "            {"
        "              \"Name\": \"Some_Container\","
        "              \"Status\": \"FINISHED\","
        "              \"Items\": ["
        "                {"
        "                  \"Name\": \"Smart Exposure\","
        "                  \"Status\": \"FINISHED\","
        "                  \"CompletedIterations\": 10,"
        "                  \"Iterations\": 10"
        "                }"
        "              ]"
        "            }"
        "          ]"
        "        }"
        "      ]"
        "    }"
        "  ]"
        "}";

    nina_client_t c;
    reset_client(&c);
    run_fetch(json, &c);

    /* find_active_target_container() falls back to the last FINISHED
     * container when nothing is RUNNING; fetch_sequence_counts_optional()
     * only takes the "authoritative new target" branch when that chosen
     * container's own Status is RUNNING, so this goes through the
     * "fill when empty" fallback instead. */
    expect_str("target_name (FINISHED fallback fill)", c.target_name, "OldTarget");
    expect_str("prev_target_container (untouched, no RUNNING claim)", c.prev_target_container, "");
    expect_str("container_name (last FINISHED container, stripped)", c.container_name, "Some");
    expect_str("container_step (nothing RUNNING -> empty)", c.container_step, "");
    expect_int("exposure_count (no RUNNING Smart Exposure -> untouched)", c.exposure_count, 0);
    expect_int("exposure_iterations (untouched)", c.exposure_iterations, 0);
    expect_str("target_time_remaining (no live conditions -> empty)", c.target_time_remaining, "");
    expect_int("target_condition_count (FINISHED subtree not scanned)", c.target_condition_count, 0);
}

/* =======================================================================
 * (c) Three levels of nested RUNNING containers under the target, bottoming
 *     out at a true leaf step (no "Items" key at all). Verifies
 *     find_active_container_name() walks to the deepest RUNNING container
 *     and find_running_step_name() walks to the actual leaf instruction
 *     rather than stopping at an intermediate container.
 * ======================================================================= */
static void test_nested_containers_deep(void) {
    printf("\n-- test_nested_containers_deep --\n");
    const char *json =
        "{"
        "  \"Response\": ["
        "    {"
        "      \"Name\": \"Targets_Container\","
        "      \"Items\": ["
        "        {"
        "          \"Name\": \"NGC7000_Container\","
        "          \"Status\": \"RUNNING\","
        "          \"Items\": ["
        "            {"
        "              \"Name\": \"Outer_Container\","
        "              \"Status\": \"RUNNING\","
        "              \"Items\": ["
        "                {"
        "                  \"Name\": \"Inner_Container\","
        "                  \"Status\": \"RUNNING\","
        "                  \"Items\": ["
        "                    { \"Name\": \"Slew To Target\", \"Status\": \"RUNNING\" }"
        "                  ]"
        "                }"
        "              ]"
        "            }"
        "          ]"
        "        }"
        "      ]"
        "    }"
        "  ]"
        "}";

    nina_client_t c;
    reset_client(&c);
    run_fetch(json, &c);

    expect_str("target_name (NGC7000_Container stripped)", c.target_name, "NGC7000");
    expect_str("container_name (deepest RUNNING container: Inner)", c.container_name, "Inner");
    expect_str("container_step (true leaf: Slew To Target)", c.container_step, "Slew To Target");
}

/* =======================================================================
 * (d) Malformed / truncated JSON must not crash: cJSON_Parse() fails and
 *     returns NULL, and fetch_sequence_counts_optional() must handle that
 *     gracefully (early return, client struct untouched). A second variant
 *     covers structurally-valid JSON with an unexpected type for "Response".
 * ======================================================================= */
static void test_malformed_json_no_crash(void) {
    printf("\n-- test_malformed_json_no_crash --\n");

    /* Truncated mid-object -> cJSON_Parse returns NULL. */
    const char *truncated =
        "{\"Response\": [ { \"Name\": \"Targets_Container\", \"Items\": [ { \"Name\": ";

    nina_client_t c;
    reset_client(&c);
    run_fetch(truncated, &c); /* must not crash */
    expect_str("target_name untouched after parse failure", c.target_name, "");
    expect_str("container_name untouched after parse failure", c.container_name, "");

    /* Valid JSON, but "Response" is a number instead of an array. */
    const char *wrong_type = "{\"Response\": 42}";
    reset_client(&c);
    run_fetch(wrong_type, &c); /* must not crash */
    expect_str("target_name untouched after wrong-type Response", c.target_name, "");
    expect_int("exposure_count untouched after wrong-type Response", c.exposure_count, 0);
}

/* =======================================================================
 * (e) Empty / null input: http_get_json() returning NULL outright (e.g.
 *     network failure), and structurally-valid JSON with an empty
 *     "Response" array (no Targets_Container present at all).
 * ======================================================================= */
static void test_empty_null_input(void) {
    printf("\n-- test_empty_null_input --\n");

    nina_client_t c;
    reset_client(&c);
    run_fetch(NULL, &c); /* s_mock_json stays NULL -> http_get_json() returns NULL */
    expect_str("target_name untouched (http_get_json NULL)", c.target_name, "");

    reset_client(&c);
    run_fetch("{\"Response\": []}", &c);
    expect_str("target_name untouched (empty Response array)", c.target_name, "");
    expect_str("container_name untouched (empty Response array)", c.container_name, "");
}

/* =======================================================================
 * (f) "_Container" suffix stripping: a target Name with no suffix is kept
 *     verbatim; a container Name carrying the suffix mid-string (not just
 *     as a trailing token) is truncated at the FIRST match, discarding
 *     everything after it — current documented behavior of
 *     strstr(out, "_Container") + truncate, not a "strip trailing token"
 *     operation.
 * ======================================================================= */
static void test_container_suffix_stripping(void) {
    printf("\n-- test_container_suffix_stripping --\n");
    const char *json =
        "{"
        "  \"Response\": ["
        "    {"
        "      \"Name\": \"Targets_Container\","
        "      \"Items\": ["
        "        {"
        "          \"Name\": \"M42\","
        "          \"Status\": \"RUNNING\","
        "          \"Items\": ["
        "            {"
        "              \"Name\": \"Foo_Container_Extra\","
        "              \"Status\": \"RUNNING\","
        "              \"Items\": ["
        "                { \"Name\": \"LeafStep\", \"Status\": \"RUNNING\" }"
        "              ]"
        "            }"
        "          ]"
        "        }"
        "      ]"
        "    }"
        "  ]"
        "}";

    nina_client_t c;
    reset_client(&c);
    run_fetch(json, &c);

    expect_str("target_name (no suffix present -> unchanged)", c.target_name, "M42");
    /* "Foo_Container_Extra" truncates at the first "_Container" match,
     * dropping "_Extra" too -- current behavior, not just suffix removal. */
    expect_str("container_name (truncates at first _Container match)", c.container_name, "Foo");
    expect_str("container_step (leaf, no suffix to strip)", c.container_step, "LeafStep");
}

/* =======================================================================
 * (g) Bonus / regression guard for a known limitation (see project memory:
 *     "NINA sequence parser Smart-Exposure-only"): find_running_smart_exposure()
 *     matches ONLY Name=="Smart Exposure" AND Status=="RUNNING". A RUNNING
 *     "Take Many Exposures" step (used by Darks/flats-style sequences) is
 *     NOT matched at current HEAD, so exposure_count/iterations/etc. stay
 *     at their prior values instead of reflecting the live step. This test
 *     asserts CURRENT behavior; it is not a fix and should be updated
 *     in lockstep if find_running_smart_exposure() is ever generalized.
 * ======================================================================= */
static void test_take_many_exposures_not_matched(void) {
    printf("\n-- test_take_many_exposures_not_matched (documents current limitation) --\n");
    const char *json =
        "{"
        "  \"Response\": ["
        "    {"
        "      \"Name\": \"Targets_Container\","
        "      \"Items\": ["
        "        {"
        "          \"Name\": \"Target1_Container\","
        "          \"Status\": \"RUNNING\","
        "          \"Items\": ["
        "            {"
        "              \"Name\": \"Take Many Exposures\","
        "              \"Status\": \"RUNNING\","
        "              \"CompletedIterations\": 5,"
        "              \"Iterations\": 20,"
        "              \"ExposureCount\": 5,"
        "              \"ExposureTime\": 60"
        "            }"
        "          ]"
        "        }"
        "      ]"
        "    }"
        "  ]"
        "}";

    nina_client_t c;
    reset_client(&c);
    run_fetch(json, &c);

    expect_str("target_name (still resolved normally)", c.target_name, "Target1");
    /* find_running_step_name() has no Name-filter, so the step label IS
     * still surfaced even though the exposure counters are not. */
    expect_str("container_step (leaf label still surfaces)", c.container_step, "Take Many Exposures");
    /* "Take Many Exposures" isn't a container (no "Items" key), so it never
     * satisfies find_active_container_name()'s container check either. */
    expect_str("container_name (not itself a container -> empty)", c.container_name, "");
    expect_int("exposure_count (NOT matched -- known limitation)", c.exposure_count, 0);
    expect_int("exposure_iterations (NOT matched -- known limitation)", c.exposure_iterations, 0);
    expect_int("exposure_total_count (NOT matched -- known limitation)", c.exposure_total_count, 0);
    expect_float("exposure_total (NOT matched -- known limitation)", c.exposure_total, 0.0f, 0.01f);
}

int main(void) {
    test_normal_running_smart_exposure();
    test_no_running_target();
    test_nested_containers_deep();
    test_malformed_json_no_crash();
    test_empty_null_input();
    test_container_suffix_stripping();
    test_take_many_exposures_not_matched();

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
