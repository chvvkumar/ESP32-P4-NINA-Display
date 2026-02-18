#pragma once

/**
 * @file nina_sequence.h
 * @brief Sequence JSON tree walkers for NINA.
 *
 * Internal header — only included by nina_client.c.
 */

#include "nina_client.h"

/**
 * @brief Try to get exposure count/iterations/time from sequence (OPTIONAL - may fail).
 * This is the only part that depends on sequence structure.
 */
void fetch_sequence_counts_optional(const char *base_url, nina_client_t *data);
