#ifndef FRAM_TEXT_H
#define FRAM_TEXT_H

#include <stdbool.h>

#include "shared/str.h"

struct Arena;

/**
 * @brief Reports whether a string contains no `NUL` byte.
 *
 * A `NUL` byte is ordinary to every first-party string. Text inputs reject it anyway, at every text
 * consumer of `os_read` for file contents, at `toml_datum_is_text` for a decoded TOML string and at
 * `toml_require_known_keys` for a decoded TOML key, because it has no meaning in any text format
 * `fram` reads or writes and an HTML parser replaces or drops it. `os_read` itself accepts any
 * bytes, because it also reads media.
 *
 * @param text Bytes to inspect.
 * @return `true` when none of the bytes is zero, or `false` otherwise.
 */
bool text_is_nul_free(struct Str text);

/**
 * @brief Reports whether `name` contains only safe template identifier bytes.
 *
 * A safe identifier is non-empty and made up only of ASCII alphanumerics, `_`, and `-`.
 *
 * @param name Candidate identifier.
 * @return `true` when `name` is a safe identifier, `false` otherwise.
 */
bool text_is_safe_identifier(struct Str name);

/**
 * @brief Converts arbitrary text to a lowercase, URL-safe slug in arena-owned storage unless it is
 *        the fallback.
 *
 * Runs of ASCII non-alphanumeric bytes collapse to single dashes. A leading dash is never emitted,
 * and a trailing one is trimmed. It folds a non-ASCII byte to two lowercase hex digits, so names
 * that differ only outside ASCII still yield distinct slugs. Text that yields no usable characters
 * falls back to the literal `untitled`.
 *
 * @param text  Source bytes to slugify.
 * @param arena Arena that owns the returned slug. Must not be `NULL`.
 * @return Slug owned by the arena, or the static fallback.
 */
struct Str text_slugify(struct Str text, struct Arena* arena) __attribute__((nonnull(2)));

#endif
