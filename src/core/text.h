#ifndef FRAM_TEXT_H
#define FRAM_TEXT_H

#include <stdbool.h>
#include <stddef.h>

struct Arena;

/**
 * @brief Duplicates a terminated string into a fresh heap buffer owned by the caller.
 *
 * This is the heap-backed counterpart to `arena_strdup`, for storage whose lifetime is not tied to
 * an arena.
 *
 * @param text Terminated source string. Must not be `NULL`.
 * @return Terminated copy the caller must `free`, or `NULL` on allocation failure.
 */
char* text_strdup(const char* text) __attribute__((nonnull(1)));

/**
 * @brief Reports whether a byte buffer contains no embedded `NUL` byte.
 *
 * Text boundaries call this to establish the codebase's `NUL`-free text invariant: every text
 * consumer of `fs_read_file`, for file contents, and `toml_datum_is_text`, for a decoded TOML
 * string. `fs_read_file` itself accepts arbitrary bytes, because it also reads media. A zero-length
 * buffer is text regardless of the pointer value.
 *
 * @param data     Bytes to inspect. May be `NULL` only when `data_len` is 0.
 * @param data_len Number of bytes to inspect.
 * @return `true` when none of the bytes is zero, or `false` otherwise.
 */
bool text_is_nul_free(const unsigned char* data, size_t data_len);

/**
 * @brief Reports whether `name` contains only safe template identifier bytes.
 *
 * A safe identifier is non-empty and made up only of ASCII alphanumerics, `_`, and `-`.
 *
 * @param name Candidate identifier. `NULL` is treated as unsafe.
 * @return `true` when `name` is a safe identifier, `false` otherwise.
 */
bool text_is_safe_identifier(const char* name);

/**
 * @brief Converts arbitrary text to a lowercase, URL-safe slug in arena-owned storage.
 *
 * Runs of ASCII non-alphanumeric bytes collapse to single dashes. A leading dash is never emitted,
 * and a trailing one is trimmed. It folds a non-ASCII byte to two lowercase hex digits, so names
 * that differ only outside ASCII still yield distinct slugs. Text that yields no usable characters
 * falls back to the literal `untitled`.
 *
 * Takes a length so a caller can slugify a borrowed slice of a larger buffer, such as one path
 * segment, without copying it out first. The returned slug is a terminated owned string either way.
 *
 * Reports the slug's length rather than leaving the caller to recover it with `strlen`. This
 * function already knows it exactly, so handing it back lets a caller that checks it against a
 * limit or sizes a joined buffer from it read that length instead of a second derivation that has
 * to agree with it.
 *
 * @param text         Source bytes to slugify. Must hold at least `text_len` bytes. Must not be
 *                     `NULL`.
 * @param text_len     Number of bytes to slugify.
 * @param arena        Arena that owns the returned slug. Must not be `NULL`.
 * @param slug_len_out Receives the slug's length in bytes, excluding the terminator. Written only
 *                     on success. Must not be `NULL`.
 * @return Terminated slug owned by the arena, or `NULL` on an oversize input or allocation failure.
 */
char* text_slugify(const char* text, size_t text_len, struct Arena* arena, size_t* slug_len_out)
    __attribute__((nonnull(1, 3, 4)));

#endif
