/**
 * @file nina_sequence.c
 * @brief Sequence JSON tree walkers for NINA.
 *
 * Handles the recursive parsing of NINA's sequence/json endpoint to
 * extract container names, step names, exposure counts, and time conditions.
 */

#include "nina_sequence.h"
#include "nina_client_internal.h"
#include "json_get.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "nina_seq";

// Parse "H:MM:SS" or "H:MM:" style RemainingTime string to total seconds.
// Returns -1 on parse failure.
static int parse_remaining_seconds(const char *str) {
    int h = 0, m = 0, s = 0;
    if (sscanf(str, "%d:%d:%d", &h, &m, &s) >= 2) {
        return h * 3600 + m * 60 + s;
    }
    return -1;
}

// Classify a condition name into a short header label for the dashboard.
static const char* classify_condition(const char *name) {
    if (!name) return "TIME LIMIT";
    if (strstr(name, "Horizon") || strstr(name, "Altitude"))
        return "SETS IN";
    if (strstr(name, "Dawn") || strstr(name, "Twilight"))
        return "DAWN IN";
    if (strstr(name, "Time"))
        return "TIME LIMIT";
    return "TIME LIMIT";
}

// State passed through the recursive search for the earliest condition.
typedef struct {
    int min_seconds;          // Smallest RemainingTime found so far (-1 = none)
    const char *reason;       // Header label for the binding constraint
    int condition_count;      // Total conditions found (including those without time)
    int64_t now_epoch;        // "Now" in the NINA clock domain (from
                              // nina_client_now_epoch); ExpectedDateTime is a
                              // NINA-PC timestamp, so the subtraction must not
                              // use the device SNTP clock. 0 = unknown.
} earliest_condition_t;

// Recursively scan all conditions with RemainingTime or ExpectedDateTime in a
// container tree, updating *out with the earliest (minimum) one found.
static void find_earliest_condition(cJSON *container, earliest_condition_t *out) {
    if (!container) return;

    // Check this container's conditions
    cJSON *conditions = cJSON_GetObjectItem(container, "Conditions");
    if (conditions && cJSON_IsArray(conditions)) {
        cJSON *cond = NULL;
        cJSON_ArrayForEach(cond, conditions) {
            out->condition_count++;

            int secs = -1;

            // Try RemainingTime first (countdown string like "H:MM:SS")
            cJSON *rem = cJSON_GetObjectItem(cond, "RemainingTime");
            if (rem && rem->valuestring && rem->valuestring[0] != '\0') {
                secs = parse_remaining_seconds(rem->valuestring);
            }

            // Fallback: compute remaining seconds from ExpectedDateTime
            // (used by Altitude/Horizon conditions that lack RemainingTime)
            if (secs < 0) {
                cJSON *edt = cJSON_GetObjectItem(cond, "ExpectedDateTime");
                if (edt && edt->valuestring && edt->valuestring[0] != '\0') {
                    time_t expected = parse_iso8601(edt->valuestring);
                    // Ignore sentinel values (year 1 / parse failure)
                    if (expected > 86400) {
                        int64_t now_nina = out->now_epoch;
                        if (now_nina > 0 && (int64_t)expected > now_nina) {
                            secs = (int)((int64_t)expected - now_nina);
                        }
                    }
                }
            }

            if (secs < 0) continue;

            if (out->min_seconds < 0 || secs < out->min_seconds) {
                out->min_seconds = secs;
                cJSON *cond_name = cJSON_GetObjectItem(cond, "Name");
                out->reason = classify_condition(
                    cond_name ? cond_name->valuestring : NULL);
            }
        }
    }

    // Recurse into RUNNING or CREATED child containers
    // (CREATED containers may have valid future time conditions)
    cJSON *items = cJSON_GetObjectItem(container, "Items");
    if (items && cJSON_IsArray(items)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, items) {
            cJSON *status = cJSON_GetObjectItem(item, "Status");
            cJSON *child_items = cJSON_GetObjectItem(item, "Items");
            if (status && status->valuestring && child_items &&
                (strcmp(status->valuestring, "RUNNING") == 0 ||
                 strcmp(status->valuestring, "CREATED") == 0)) {
                find_earliest_condition(item, out);
            }
        }
    }
}

// Find the active target container (RUNNING preferred, otherwise last FINISHED)
static cJSON* find_active_target_container(cJSON *targets_container) {
    cJSON *items = cJSON_GetObjectItem(targets_container, "Items");
    if (!items || !cJSON_IsArray(items)) return NULL;

    cJSON *running = NULL;
    cJSON *last_finished = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *status = cJSON_GetObjectItem(item, "Status");
        if (!status || !status->valuestring) continue;
        if (strcmp(status->valuestring, "RUNNING") == 0) {
            running = item;
            break;
        }
        if (strcmp(status->valuestring, "FINISHED") == 0) {
            last_finished = item;
        }
    }
    return running ? running : last_finished;
}

// Find the deepest RUNNING container name, or fall back to last FINISHED container
static void find_active_container_name(cJSON *parent, char *out, size_t out_size) {
    if (!parent) return;

    cJSON *items = cJSON_GetObjectItem(parent, "Items");
    if (!items || !cJSON_IsArray(items)) return;

    // First pass: look for a RUNNING container child
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_status = cJSON_GetObjectItem(item, "Status");
        cJSON *item_name = cJSON_GetObjectItem(item, "Name");
        cJSON *item_items = cJSON_GetObjectItem(item, "Items");
        if (!item_status || !item_status->valuestring || !item_name || !item_name->valuestring) continue;
        if (!item_items || !cJSON_IsArray(item_items)) continue;

        if (strcmp(item_status->valuestring, "RUNNING") == 0) {
            strncpy(out, item_name->valuestring, out_size - 1);
            out[out_size - 1] = '\0';
            char *suffix = strstr(out, "_Container");
            if (suffix) *suffix = '\0';
            // Try to find a deeper RUNNING container
            char deeper[64] = {0};
            find_active_container_name(item, deeper, sizeof(deeper));
            if (deeper[0] != '\0') {
                strncpy(out, deeper, out_size - 1);
                out[out_size - 1] = '\0';
            }
            return;
        }
    }

    // Second pass: no RUNNING container, use last FINISHED container
    cJSON *last_finished = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_status = cJSON_GetObjectItem(item, "Status");
        cJSON *item_items = cJSON_GetObjectItem(item, "Items");
        if (!item_status || !item_status->valuestring) continue;
        if (!item_items || !cJSON_IsArray(item_items)) continue;
        if (strcmp(item_status->valuestring, "FINISHED") == 0) {
            last_finished = item;
        }
    }
    if (last_finished) {
        cJSON *item_name = cJSON_GetObjectItem(last_finished, "Name");
        if (item_name && item_name->valuestring) {
            strncpy(out, item_name->valuestring, out_size - 1);
            out[out_size - 1] = '\0';
            char *suffix = strstr(out, "_Container");
            if (suffix) *suffix = '\0';
        }
    }
}

// Find the currently running step (leaf instruction) within a container tree
static void find_running_step_name(cJSON *parent, char *out, size_t out_size) {
    if (!parent) return;

    cJSON *items = cJSON_GetObjectItem(parent, "Items");
    if (!items || !cJSON_IsArray(items)) return;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_status = cJSON_GetObjectItem(item, "Status");
        cJSON *item_name = cJSON_GetObjectItem(item, "Name");
        if (!item_status || !item_status->valuestring || !item_name || !item_name->valuestring) continue;

        if (strcmp(item_status->valuestring, "RUNNING") == 0) {
            cJSON *child_items = cJSON_GetObjectItem(item, "Items");
            if (child_items && cJSON_IsArray(child_items) && cJSON_GetArraySize(child_items) > 0) {
                // This is a container - recurse deeper
                find_running_step_name(item, out, out_size);
                if (out[0] == '\0') {
                    strncpy(out, item_name->valuestring, out_size - 1);
                    out[out_size - 1] = '\0';
                    char *suffix = strstr(out, "_Container");
                    if (suffix) *suffix = '\0';
                }
            } else {
                // Leaf instruction - this is the current step
                strncpy(out, item_name->valuestring, out_size - 1);
                out[out_size - 1] = '\0';
            }
            return;
        }
    }
}

// Nested exposure progress accumulated while unwinding from the RUNNING
// Smart Exposure out to the target container. Starts at the Smart Exposure's
// own Iterations / CompletedIterations; every enclosing container that carries
// a "Loop For Iterations" condition multiplies the target and adds its
// completed passes scaled by the inner target.
#define NESTED_EXPOSURE_TARGET_MAX 10000
typedef struct {
    int target;   // Iterations x product of enclosing loop Iterations
    int done;     // Completed exposures in that flattened count
    cJSON *parent; // Container that DIRECTLY holds the RUNNING Smart Exposure
                   // (the one whose plan the sub bar draws). NULL until found.
} nested_exposure_t;

// Fold this container's "Loop For Iterations" condition(s) into *acc.
// Iterations <= 0 is treated as "not a loop" (factor 1).
static void apply_loop_conditions(cJSON *container, nested_exposure_t *acc) {
    cJSON *conditions = cJSON_GetObjectItem(container, "Conditions");
    if (!conditions || !cJSON_IsArray(conditions)) return;
    cJSON *cond = NULL;
    cJSON_ArrayForEach(cond, conditions) {
        cJSON *name = cJSON_GetObjectItem(cond, "Name");
        if (!name || !name->valuestring ||
            strncmp(name->valuestring, "Loop For Iterations", 19) != 0) continue;
        int iters = 0, completed = 0;
        JSON_GET_INT(cond, "Iterations", iters);
        JSON_GET_INT(cond, "CompletedIterations", completed);
        if (iters <= 1) continue;
        if (completed < 0) completed = 0;
        if (completed > iters) completed = iters;
        if ((int64_t)acc->target * iters > NESTED_EXPOSURE_TARGET_MAX) {
            acc->target = NESTED_EXPOSURE_TARGET_MAX;
            continue;
        }
        acc->done += completed * acc->target;
        acc->target *= iters;
    }
}

// Recursively search for the RUNNING Smart Exposure. When found, every
// container on the path back out (including the one passed in) folds its loop
// condition into *acc, so only the enclosing path contributes.
static cJSON* find_running_smart_exposure(cJSON *container, nested_exposure_t *acc) {
    if (!container) return NULL;

    cJSON *items = cJSON_GetObjectItem(container, "Items");
    if (!items || !cJSON_IsArray(items)) return NULL;

    cJSON *found = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_name = cJSON_GetObjectItem(item, "Name");
        cJSON *item_status = cJSON_GetObjectItem(item, "Status");

        if (item_name && item_name->valuestring &&
            strcmp(item_name->valuestring, "Smart Exposure") == 0) {
            if (item_status && item_status->valuestring &&
                strcmp(item_status->valuestring, "RUNNING") == 0) {
                found = item;
                acc->parent = container;
                JSON_GET_INT(found, "Iterations", acc->target);
                JSON_GET_INT(found, "CompletedIterations", acc->done);
                if (acc->target < 1) acc->target = 1;
                break;
            }
        }

        found = find_running_smart_exposure(item, acc);
        if (found) break;
    }

    if (found) apply_loop_conditions(container, acc);
    return found;
}

// Read the container plan off @p parent, the container that directly holds the
// RUNNING Smart Exposure @p running. One item per Smart Exposure in Items
// order (tracked up to NINA_PLAN_MAX_ITEMS); the container's "Loop For
// Iterations" condition gives rounds and rounds already done; total_images
// counts EVERY Smart Exposure the container holds, tracked or not, so the
// single-image hide rule stays right past the cap. A DISABLED item, or one
// with Iterations <= 0, becomes a one-image placeholder that never fills.
static void build_plan(cJSON *parent, cJSON *running, nina_plan_t *out) {
    memset(out, 0, sizeof(*out));
    out->rounds = 1;
    out->running_idx = -1;
    if (!parent) return;

    cJSON *items = cJSON_GetObjectItem(parent, "Items");
    if (!items || !cJSON_IsArray(items)) return;

    int count = 0;   // Smart Exposures seen, including any past the cap
    int images = 0;  // Images per round across all of them
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_name = cJSON_GetObjectItem(item, "Name");
        if (!cJSON_IsString(item_name) ||
            strcmp(item_name->valuestring, "Smart Exposure") != 0) continue;

        int iters = 0, completed = 0;
        JSON_GET_INT(item, "Iterations", iters);
        JSON_GET_INT(item, "CompletedIterations", completed);

        cJSON *item_status = cJSON_GetObjectItem(item, "Status");
        bool enabled = (iters > 0) &&
                       !(cJSON_IsString(item_status) &&
                         strcmp(item_status->valuestring, "DISABLED") == 0);
        if (!enabled) {
            iters = 1;      // one placeholder image wide
            completed = 0;  // and it never fills
        }
        if (completed < 0) completed = 0;
        if (completed > iters) completed = iters;

        if (count < NINA_PLAN_MAX_ITEMS) {
            nina_plan_item_t *slot = &out->items[count];
            JSON_GET_STR(item, "Filter", slot->filter);
            slot->iterations = iters;
            slot->completed  = completed;
            slot->enabled    = enabled;
            slot->running    = (item == running);
            if (slot->running) out->running_idx = count;
        }
        images += iters;
        count++;
    }

    out->n_items = (count > NINA_PLAN_MAX_ITEMS) ? NINA_PLAN_MAX_ITEMS : count;

    cJSON *conditions = cJSON_GetObjectItem(parent, "Conditions");
    if (conditions && cJSON_IsArray(conditions)) {
        cJSON *cond = NULL;
        cJSON_ArrayForEach(cond, conditions) {
            cJSON *cond_name = cJSON_GetObjectItem(cond, "Name");
            if (!cJSON_IsString(cond_name) ||
                strncmp(cond_name->valuestring, "Loop For Iterations", 19) != 0) continue;
            int rounds = 0, done = 0;
            JSON_GET_INT(cond, "Iterations", rounds);
            JSON_GET_INT(cond, "CompletedIterations", done);
            if (rounds <= 1) continue;   // not a real loop: one round
            if (done < 0) done = 0;
            if (done > rounds - 1) done = rounds - 1;
            out->rounds = rounds;
            out->round_done = done;
            break;
        }
    }

    out->total_images = out->rounds * images;

    cJSON *parent_name = cJSON_GetObjectItem(parent, "Name");
    if (cJSON_IsString(parent_name)) {
        strncpy(out->container, parent_name->valuestring, sizeof(out->container) - 1);
        out->container[sizeof(out->container) - 1] = '\0';
        char *suffix = strstr(out->container, "_Container");
        if (suffix) *suffix = '\0';
    }
}

void fetch_sequence_counts_optional(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%ssequence/json", base_url);

    cJSON *json = http_get_json(url);
    if (!json) {
        ESP_LOGW(TAG, "Sequence data unavailable - exposure counts will not be shown");
        return;
    }

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (!response || !cJSON_IsArray(response)) {
        cJSON_Delete(json);
        return;
    }

    // Search for Targets_Container
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, response) {
        cJSON *name = cJSON_GetObjectItem(item, "Name");
        if (name && name->valuestring && strcmp(name->valuestring, "Targets_Container") == 0) {
            cJSON *items = cJSON_GetObjectItem(item, "Items");
            if (items && cJSON_IsArray(items) && cJSON_GetArraySize(items) > 0) {
                cJSON *target_container = find_active_target_container(item);
                if (!target_container) target_container = cJSON_GetArrayItem(items, 0);

                // Is the chosen container actually RUNNING? (find_active_target_container can
                // return a FINISHED container as fallback; only a RUNNING one is a real active
                // target.) Also gates whether an idle gap keeps the container plan alive.
                cJSON *tc_status = cJSON_GetObjectItem(target_container, "Status");
                bool tc_running = (tc_status && tc_status->valuestring &&
                                   strcmp(tc_status->valuestring, "RUNNING") == 0);

                // Resolve target name from the active target container.
                cJSON *target_name_json = cJSON_GetObjectItem(target_container, "Name");
                if (target_name_json && target_name_json->valuestring) {
                    char temp_name[64];
                    strlcpy(temp_name, target_name_json->valuestring, sizeof(temp_name));

                    char *suffix = strstr(temp_name, "_Container");
                    if (suffix) *suffix = '\0';

                    if (temp_name[0] != '\0' && tc_running &&
                        strcmp(temp_name, data->prev_target_container) != 0) {
                        // New RUNNING target container detected -> authoritative update at sequence/target start
                        strlcpy(data->target_name, temp_name, sizeof(data->target_name));
                        strlcpy(data->prev_target_container, temp_name, sizeof(data->prev_target_container));
                        ESP_LOGI(TAG, "Target (new RUNNING container): %s", data->target_name);
                    } else if (temp_name[0] != '\0' &&
                               (data->target_name[0] == '\0' ||
                                strcmp(data->target_name, "No Target") == 0)) {
                        // Existing fallback: fill when we have no name yet (idle/CREATED/boot)
                        strlcpy(data->target_name, temp_name, sizeof(data->target_name));
                        ESP_LOGI(TAG, "Target (fallback from sequence): %s", data->target_name);
                    }
                }

                // Find the active container name
                find_active_container_name(target_container, data->container_name, sizeof(data->container_name));
                if (data->container_name[0] != '\0') {
                    ESP_LOGI(TAG, "Active container: %s", data->container_name);
                }

                // Find the earliest binding condition (time, horizon, dawn, etc.)
                data->target_time_remaining[0] = '\0';
                data->target_time_reason[0] = '\0';
                /* now_epoch: NINA clock domain (falls back to time(NULL) while
                 * unknown). This runs on the instance's poll task — the same
                 * task that writes the clock pair in the camera-info fetcher —
                 * so the unlocked read cannot tear. */
                earliest_condition_t earliest = { .min_seconds = -1, .reason = "TIME LIMIT",
                                                  .condition_count = 0,
                                                  .now_epoch = nina_client_now_epoch(data) };
                find_earliest_condition(target_container, &earliest);
                data->target_condition_count = earliest.condition_count;
                if (earliest.min_seconds >= 0) {
                    int h = earliest.min_seconds / 3600;
                    int m = (earliest.min_seconds % 3600) / 60;
                    snprintf(data->target_time_remaining,
                             sizeof(data->target_time_remaining),
                             "%dh %02dm", h, m);
                    strlcpy(data->target_time_reason, earliest.reason,
                            sizeof(data->target_time_reason));
                    ESP_LOGI(TAG, "Earliest condition: %s %s (%s)",
                             data->target_time_remaining, data->target_time_reason,
                             earliest.reason);
                }

                // Find the currently running step
                data->container_step[0] = '\0';
                find_running_step_name(target_container, data->container_step, sizeof(data->container_step));
                if (data->container_step[0] != '\0') {
                    ESP_LOGI(TAG, "Active step: %s", data->container_step);
                }

                // Recursively search for RUNNING Smart Exposure
                // (target/done folded with enclosing "Loop For Iterations" conditions)
                nested_exposure_t nested = { .target = 0, .done = 0, .parent = NULL };
                cJSON *running_exp = find_running_smart_exposure(target_container, &nested);

                if (running_exp) {
                    if (nested.done < 0) nested.done = 0;
                    if (nested.done > nested.target) nested.done = nested.target;
                    data->exposure_count = nested.done;
                    data->exposure_iterations = nested.target;
                    build_plan(nested.parent, running_exp, &data->plan);
                    JSON_GET_INT(running_exp, "ExposureCount", data->exposure_total_count);

                    cJSON *exp_time = cJSON_GetObjectItem(running_exp, "ExposureTime");
                    if (exp_time && cJSON_IsNumber(exp_time)) {
                        data->exposure_total = (float)exp_time->valuedouble;
                        ESP_LOGI(TAG, "ExposureTime (from running sequence): %.1fs", data->exposure_total);
                    }

                    ESP_LOGI(TAG, "Exposure count: %d/%d", data->exposure_count, data->exposure_iterations);

                    /* Fallback step name: find_running_step_name() can come up
                     * empty on a nested Smart-Exposure-in-Loop sequence even
                     * though the running exposure was located. Name the step
                     * from that node so the UI shows the active instruction. */
                    if (data->container_step[0] == '\0') {
                        cJSON *rname = cJSON_GetObjectItem(running_exp, "Name");
                        if (rname && cJSON_IsString(rname) && rname->valuestring) {
                            strncpy(data->container_step, rname->valuestring,
                                    sizeof(data->container_step) - 1);
                            data->container_step[sizeof(data->container_step) - 1] = '\0';
                            char *suffix = strstr(data->container_step, "_Container");
                            if (suffix) *suffix = '\0';
                        }
                    }
                } else {
                    /* Nothing exposing right now. While the SAME container is
                     * still RUNNING (container_name was just resolved above
                     * as the deepest RUNNING container, which is the parent of
                     * the exposures) this is just a gap (autofocus, dither,
                     * wait), so keep the plan on screen and only drop the
                     * running mark. Once that container stops, or the target
                     * moves on to another one, the plan goes away with it. */
                    if (tc_running && data->plan.container[0] != '\0' &&
                        strcmp(data->container_name, data->plan.container) == 0) {
                        data->plan.running_idx = -1;
                        for (int i = 0; i < data->plan.n_items; i++) {
                            data->plan.items[i].running = false;
                        }
                    } else {
                        memset(&data->plan, 0, sizeof(data->plan));
                    }
                    ESP_LOGD(TAG, "No RUNNING Smart Exposure found (sequence may be idle)");
                }
            }
            break;
        }
    }

    cJSON_Delete(json);
}
