#ifndef FRAM_TOML_H
#define FRAM_TOML_H

#include <stdbool.h>
#include <stddef.h>
#include <tomlc17.h>

#include "shared/str.h"

struct Log;

// Every function here, and every caller that threads a datum onward, takes `toml_datum_t` by value
// rather than by pointer, against the general rule that a non-trivial struct is passed by pointer.
// This is the tomlc17 boundary. `toml_get` returns a datum by value (see
// `vendor/tomlc17/tomlc17.h`), so the alternative is a local copy per call whose only purpose is to
// have an address to take. Matching the library's shape at its own boundary keeps the translation
// in one layer, which is where the codebase confines every other external convention. Do not spread
// the by-value habit to structs this repository owns.

/**
 * @brief Parses a TOML document, treating an allocation failure inside tomlc17 as out of memory and
 *        logging a parse error.
 *
 * This is the one entry point to the tomlc17 parser, so every caller gets the project's allocation
 * policy. tomlc17 allocates through `malloc` and reports a failed allocation as a parse error. The
 * first call installs an allocator that calls `os_oom` instead, so a returned error is always a
 * problem with the document. The installation happens once, under `pthread_once`, which makes the
 * call safe from concurrent pool jobs.
 *
 * This is also where tomlc17's `ok` flag becomes the project's `0`/`-1`: a parse error is logged
 * against `file_path` and its result is released here, so a caller sees either a result to read or
 * nothing to free.
 *
 * tomlc17 reads exactly `document.len` bytes and needs no terminator. It takes the length as an
 * `int`, so the document must be no longer than `INT_MAX` bytes, which every caller's input limit
 * guarantees.
 *
 * @param document   Document bytes. `data` must not be `NULL`.
 * @param file_path  File the document came from, named in the parse error.
 * @param parsed_out Receives the parse result on success, which the caller must pass to `toml_free`
 *                   exactly once. Must not be `NULL`.
 * @param operation  What failed, the message's prefix, such as `failed to parse config`.
 * @param log        Log that receives the parse error. Must not be `NULL`.
 * @return `0` on success, or `-1` on a parse error.
 */
int toml_parse_document(struct Str document,
                        struct Str file_path,
                        toml_result_t* parsed_out,
                        struct Str operation,
                        struct Log* log) __attribute__((nonnull(3, 5)));

/**
 * @brief Reports whether a datum is a string whose bytes hold no embedded `NUL`.
 *
 * TOML permits an escape that decodes to `U+0000`, and tomlc17 encodes it as a `NUL` byte inside an
 * otherwise ordinary string value. Every caller that turns a TOML string into a configuration value
 * checks this first, which keeps the input policy of `text_is_nul_free`.
 *
 * @param value Datum to classify.
 * @return `true` when `value` is a string with no embedded `NUL`, `false` otherwise.
 */
bool toml_datum_is_text(toml_datum_t value);

/**
 * @brief Returns a view of a string datum's bytes.
 *
 * The view borrows the bytes: they point into the `toml_result_t`'s own storage and die with
 * `toml_free`, so a caller that keeps the value clones it first.
 *
 * @param value Datum to view. Must be a `TOML_STRING` datum.
 * @return View of the string's bytes, without tomlc17's terminator.
 */
struct Str toml_datum_str(toml_datum_t value);

/**
 * @brief Rejects every key of a table that its level of the schema does not define.
 *
 * A loader passes every key it reads, before it reads any, so a misspelled key fails the load
 * rather than silently leaving its default in place. Keys match exactly, so a key that is a prefix
 * of a known key, or extends one, is as unknown as any other. Every unknown key is named. A quoted
 * key holding a `NUL` decoded from an escape is one of those unknown keys, but it is reported
 * without its name, which `%.*s` would cut at the `NUL`: as
 * `<key_kind> key must not contain a NUL byte` in the top-level table, and as
 * `<key_kind> key under '<table>' must not contain a NUL byte` in a nested one.
 *
 * @param table           Parsed TOML table. Must be a `TOML_TABLE` datum.
 * @param known_keys      Keys the table may hold. May be `NULL` only when `known_key_count` is 0.
 * @param known_key_count Number of keys in `known_keys`.
 * @param key_kind        Kind of key the diagnostic names, such as `config`.
 * @param key_prefix      Dotted path of `table` with a trailing `.`, such as `section.table.`, or
 *                        empty for the top-level table. It qualifies the key in the diagnostic.
 * @param file_path       File the table came from, named in each error, or empty to name none.
 * @param log             Log that receives `unknown <key_kind> key '<key_prefix><key>'` for each
 *                        unknown key, in table order. Must not be `NULL`.
 * @return `0` when every key is known, or `-1` when any is not.
 */
int toml_require_known_keys(toml_datum_t table,
                            const struct Str* known_keys,
                            ptrdiff_t known_key_count,
                            struct Str key_kind,
                            struct Str key_prefix,
                            struct Str file_path,
                            struct Log* log) __attribute__((nonnull(7)));

#endif
