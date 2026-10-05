#ifndef FRAM_PARSE_H
#define FRAM_PARSE_H

#include <stddef.h>

/**
 * @brief Parses a non-negative decimal integer that fits in `size_t`.
 *
 * Accepts only a bare run of ASCII digits. It rejects a leading sign, leading whitespace, empty
 * input, trailing characters, and values that overflow `size_t`. Callers that need a positive value
 * reject zero themselves.
 *
 * @param text      Terminated candidate text. Must not be `NULL`.
 * @param value_out Receives the parsed value on success. Left untouched on failure. Must not be
 *                  `NULL`.
 * @return `0` on success, or `-1` when `text` is not a non-negative in-range integer. The largest
 *         accepted value is `SIZE_MAX`.
 */
int parse_size(const char* text, size_t* value_out) __attribute__((nonnull(1, 2)));

#endif
