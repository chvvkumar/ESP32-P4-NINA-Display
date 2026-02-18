/**
 * @file nina_sequence.c
 * @brief Sequence JSON tree walkers for NINA.
 *
 * Handles the recursive parsing of NINA's sequence/json endpoint to
 * extract container names, step names, exposure counts, and time conditions.
 */

#include "nina_sequence.h"
#include "nina_client_internal.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "nina_seq";

// Recursively find Loop Until Time_Condition in a container or its RUNNING children
static cJSON* find_time_condition(cJSON *container) {
    if (!container) return NULL;

    // Check this container's conditions
    cJSON *conditions = cJSON_GetObjectItem(container, "Conditions");
    if (conditions && cJSON_IsArray(conditions)) {
        cJSON *cond = NULL;
        cJSON_ArrayForEach(cond, conditions) {
            cJSON *cond_name = cJSON_GetObjectItem(cond, "Name");
            if (cond_name && cond_name->valuestring &&
                strcmp(cond_name->valuestring, "Loop Until Time_Condition") == 0) {
                return cond;
            }
        }
    }

    // Search RUNNING child containers
    cJSON *items = cJSON_GetObjectItem(container, "Items");
    if (items && cJSON_IsArray(items)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, items) {
            cJSON *status = cJSON_GetObjectItem(item, "Status");
            cJSON *child_items = cJSON_GetObjectItem(item, "Items");
            if (status && status->valuestring && child_items &&
                strcmp(status->valuestring, "RUNNING") == 0) {
                cJSON *found = find_time_condition(item);
                if (found) return found;
            }
        }
    }
    return NULL;
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

// Helper function to recursively search for RUNNING Smart Exposure
static cJSON* find_running_smart_exposure(cJSON *container) {
    if (!container) return NULL;

    cJSON *items = cJSON_GetObjectItem(container, "Items");
    if (!items || !cJSON_IsArray(items)) return NULL;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_name = cJSON_GetObjectItem(item, "Name");
        cJSON *item_status = cJSON_GetObjectItem(item, "Status");

        if (item_name && item_name->valuestring &&
            strcmp(item_name->valuestring, "Smart Exposure") == 0) {
            if (item_status && item_status->valuestring &&
                strcmp(item_status->valuestring, "RUNNING") == 0) {
                return item;
            }
        }

        cJSON *nested = find_running_smart_exposure(item);
        if (nested) return nested;
    }

    return NULL;
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

                // Use container name as fallback target name
                cJSON *target_name_json = cJSON_GetObjectItem(target_container, "Name");
                if (target_name_json && target_name_json->valuestring) {
                    char temp_name[64];
                    strlcpy(temp_name, target_name_json->valuestring, sizeof(temp_name));

                    char *suffix = strstr(temp_name, "_Container");
                    if (suffix) *suffix = '\0';

                    if (temp_name[0] != '\0' &&
                        (data->target_name[0] == '\0' ||
                         strcmp(data->target_name, "No Target") == 0)) {
                        strlcpy(data->target_name, temp_name, sizeof(data->target_name));
                        ESP_LOGI(TAG, "Target (fallback from sequence): %s", data->target_name);
                    }
                }

                // Find the active container name
                find_active_container_name(target_container, data->container_name, sizeof(data->container_name));
                if (data->container_name[0] != '\0') {
                    ESP_LOGI(TAG, "Active container: %s", data->container_name);
                }

                // Extract target remaining time
                data->target_time_remaining[0] = '\0';
                cJSON *time_cond = find_time_condition(target_container);
                if (time_cond) {
                    cJSON *rem = cJSON_GetObjectItem(time_cond, "RemainingTime");
                    if (rem && rem->valuestring) {
                        int h = 0, m = 0;
                        if (sscanf(rem->valuestring, "%d:%d:", &h, &m) >= 2) {
                            snprintf(data->target_time_remaining,
                                     sizeof(data->target_time_remaining),
                                     "%d:%02d", h, m);
                            ESP_LOGI(TAG, "Target time remaining: %s", data->target_time_remaining);
                        }
                    }
                }

                // Find the currently running step
                data->container_step[0] = '\0';
                find_running_step_name(target_container, data->container_step, sizeof(data->container_step));
                if (data->container_step[0] != '\0') {
                    ESP_LOGI(TAG, "Active step: %s", data->container_step);
                }

                // Recursively search for RUNNING Smart Exposure
                cJSON *running_exp = find_running_smart_exposure(target_container);

                if (running_exp) {
                    cJSON *completed = cJSON_GetObjectItem(running_exp, "CompletedIterations");
                    if (completed && cJSON_IsNumber(completed)) {
                        data->exposure_count = completed->valueint;
                    }

                    cJSON *iterations = cJSON_GetObjectItem(running_exp, "Iterations");
                    if (iterations && cJSON_IsNumber(iterations)) {
                        data->exposure_iterations = iterations->valueint;
                    }

                    cJSON *exp_time = cJSON_GetObjectItem(running_exp, "ExposureTime");
                    if (exp_time && cJSON_IsNumber(exp_time)) {
                        data->exposure_total = (float)exp_time->valuedouble;
                        ESP_LOGI(TAG, "ExposureTime (from running sequence): %.1fs", data->exposure_total);
                    }

                    ESP_LOGI(TAG, "Exposure count: %d/%d", data->exposure_count, data->exposure_iterations);
                } else {
                    ESP_LOGD(TAG, "No RUNNING Smart Exposure found (sequence may be idle)");
                }
            }
            break;
        }
    }

    cJSON_Delete(json);
}
