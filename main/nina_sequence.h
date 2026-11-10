#pragma once

/**
 * @file nina_sequence.h
 * @brief Sequence JSON tree walkers for NINA.
 *
 * Internal header — only included by nina_client.c.
 */

#include "nina_client.h"
#include "cJSON.h"
#include <stdbool.h>

/**
 * @brief Try to get exposure count/iterations/time from sequence (OPTIONAL - may fail).
 * This is the only part that depends on sequence structure.
 */
void fetch_sequence_counts_optional(const char *base_url, nina_client_t *data);

/**
 * @brief Target RA (hours) and Dec (degrees) of the target container named
 * @p container_name (full name, "_Container" suffix included) from a
 * sequence/state "Response" array: Targets_Container -> Items -> that
 * container -> Target.InputCoordinates {RAHours, RAMinutes, RASeconds,
 * NegativeDec, DecDegrees, DecMinutes, DecSeconds}. Pure; host-tested.
 * @return true when found and both RAHours and DecDegrees are numbers.
 */
bool nina_seq_state_target_coords(const cJSON *response, const char *container_name,
                                  float *ra_h, float *dec_deg);
