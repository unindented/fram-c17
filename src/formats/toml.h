#ifndef FRAM_TOML_H
#define FRAM_TOML_H

#include <stdbool.h>
#include <stddef.h>
#include <tomlc17.h>

// Every function here, and every caller that threads a datum onward, takes `toml_datum_t` by value
// rather than by pointer, against the general rule that a non-trivial struct is passed by pointer.
// This is the tomlc17 boundary. `toml_get` returns a datum by value (see
// `vendor/tomlc17/tomlc17.h`), so the alternative is a local copy per call whose only purpose is to
// have an address to take. Matching the library's shape at its own boundary keeps the translation
// in one layer, which is where the codebase confines every other external convention. Do not spread
// the by-value habit to structs this repository owns.

/**
 * @brief Reports whether a datum is a string whose bytes hold no embedded `NUL`.
 *
 * TOML permits an escape that decodes to `U+0000`, and tomlc17 encodes it as a `NUL` byte inside an
 * otherwise ordinary string value. Copying such a value would break the codebase's text invariant
 * (see `text_is_nul_free`) and silently truncate the rendered output at the `NUL`, so every caller
 * that turns a TOML string into an owned string checks this first.
 *
 * A datum's bytes are borrowed, not owned. `value.u.str.ptr` points into the `toml_result_t`'s own
 * storage and dies with `toml_free`, and `len` excludes the terminator. A caller that stored that
 * pointer rather than copying the bytes would compile, pass its test, and dangle the moment the
 * parse result was released.
 *
 * @param value Datum to classify.
 * @return `true` when `value` is a string with no embedded `NUL`, `false` otherwise.
 */
bool toml_datum_is_text(toml_datum_t value);

/**
 * @brief Rejects the first key of a table that its level of the schema does not define.
 *
 * A loader passes every key it reads, before it reads any, so a misspelled key fails the load
 * rather than silently leaving its default in place. Keys match exactly, so a key that is a prefix
 * of a known key, or extends one, is as unknown as any other.
 *
 * @param table           Parsed TOML table. Must be a `TOML_TABLE` datum.
 * @param known_keys      Keys the table may hold. May be `NULL` only when `known_key_count` is 0.
 * @param known_key_count Number of keys in `known_keys`.
 * @param key_kind        Kind of key the diagnostic names, such as `config`. Must not be `NULL`.
 * @param key_prefix      Dotted path of `table` with a trailing `.`, such as `section.table.`, or
 *                        `""` for the top-level table. It qualifies the key in the diagnostic. Must
 *                        not be `NULL`.
 * @param err             Buffer for a diagnostic message on failure. May be `NULL` only when
 *                        `err_len` is 0.
 * @param err_len         Size of `err` in bytes.
 * @return `0` when every key is known, or `-1` on the first unknown key, with
 *         `unknown <key_kind> key '<key_prefix><key>'` in `err`.
 */
int toml_require_known_keys(toml_datum_t table,
                            const char* const* known_keys,
                            size_t known_key_count,
                            const char* key_kind,
                            const char* key_prefix,
                            char* err,
                            size_t err_len) __attribute__((nonnull(4, 5)));

#endif
