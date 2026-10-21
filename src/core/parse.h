#ifndef FRAM_PARSE_H
#define FRAM_PARSE_H

#include <stddef.h>

#include "shared/str.h"

/**
 * @brief Parses a non-negative decimal integer that fits in `size_t`, saturated at `PTRDIFF_MAX`.
 *
 * Accepts only a bare run of ASCII digits. It rejects a leading sign, leading whitespace, empty
 * input, trailing characters, and values that overflow `size_t`. A value above `PTRDIFF_MAX` gives
 * `PTRDIFF_MAX`, so it is still a number, which a caller with an upper limit rejects with the
 * limit's own message. Callers that need a positive value reject zero themselves.
 *
 * @param text      Candidate text.
 * @param value_out Receives the parsed value on success. Left untouched on failure. Must not be
 *                  `NULL`.
 * @return `0` on success, or `-1` when `text` is not a non-negative integer up to `SIZE_MAX`.
 */
int parse_size(struct Str text, ptrdiff_t* value_out) __attribute__((nonnull(2)));

#endif
