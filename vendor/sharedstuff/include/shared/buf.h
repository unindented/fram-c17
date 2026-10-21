#ifndef SHARED_BUF_H
#define SHARED_BUF_H

#include <stdarg.h>
#include <stddef.h>

#include "shared/str.h"

struct Arena;

/**
 * Growable output buffer whose storage comes from an arena.
 *
 * Start one as `(struct Buf){.arena = arena}`. While `data` is the arena's most recent allocation,
 * growing extends it in place. Its storage is not zeroed, so a doubled capacity the buffer never
 * writes costs address space but no memory. Otherwise the bytes move to the end of the arena, and
 * the old copy stays valid but unused until the arena's region is released. Nothing is freed, so a
 * pointer into the buffer taken before a growth still reads the bytes it pointed at, though later
 * appends do not reach it.
 */
struct Buf {
  /** First byte of the buffer, or `NULL` before the first append. */
  char* data;

  /** Number of bytes appended. */
  ptrdiff_t len;

  /** Bytes reserved at `data`. */
  ptrdiff_t cap;

  /** Arena the buffer grows in. Must outlive the buffer. */
  struct Arena* arena;
};

/**
 * @brief Appends a string's bytes.
 *
 * The source may point into the buffer itself, because growth never frees the old bytes.
 *
 * @param buffer Buffer to append to. Must not be `NULL`.
 * @param s      Bytes to append.
 */
void buf_append(struct Buf* buffer, struct Str s) __attribute__((nonnull(1)));

/**
 * @brief Appends a single byte.
 *
 * @param buffer Buffer to append to. Must not be `NULL`.
 * @param c      Byte to append.
 */
void buf_append_char(struct Buf* buffer, char c) __attribute__((nonnull(1)));

/**
 * @brief Appends the text that a `printf` format and its arguments produce.
 *
 * Use it as `buf_printf(&buffer, "read '%.*s' at %td", STR_FMT(path), offset)`. The compiler checks
 * the arguments against the format, so the format must be a string literal. The text is never
 * truncated: the buffer grows to hold all of it. A `%.*s` argument may point into the buffer
 * itself, because growth never frees the old bytes.
 *
 * @param buffer Buffer to append to. Must not be `NULL`.
 * @param format `printf` format. Must not be `NULL`.
 * @param ...    Arguments that the format converts.
 */
void buf_printf(struct Buf* buffer, const char* format, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Appends the text that a `printf` format and a `va_list` produce, as `buf_printf` does.
 *
 * This is the form for a function that forwards its own format and arguments. That function must
 * carry `__attribute__((format(printf, n, m)))` itself, so the compiler checks its callers.
 *
 * @param buffer Buffer to append to. Must not be `NULL`.
 * @param format `printf` format. Must not be `NULL`.
 * @param args   Arguments that the format converts. This call consumes them.
 */
void buf_vprintf(struct Buf* buffer, const char* format, va_list args)
    __attribute__((format(printf, 2, 0), nonnull(1, 2)));

/**
 * @brief Returns the bytes appended so far.
 *
 * The view stays valid after later appends, but does not include them.
 *
 * @param buffer Buffer to read. Must not be `NULL`.
 * @return View of `buffer->len` bytes at `buffer->data`.
 */
struct Str buf_str(const struct Buf* buffer) __attribute__((nonnull(1)));

/**
 * @brief Terminates the buffer after its last byte and returns it as a C string.
 *
 * The terminator is not counted in `len`, so the buffer can keep growing afterwards, and an empty
 * buffer yields `""` rather than `NULL`.
 *
 * @param buffer Buffer to terminate. Must not be `NULL`.
 * @return `buffer->data`, terminated at `buffer->len`.
 */
char* buf_cstr(struct Buf* buffer) __attribute__((nonnull(1), returns_nonnull));

#endif
