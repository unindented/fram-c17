#include "shared/buf.h"

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "shared/arena.h"

/**
 * @brief Ensures the buffer has room for `need` more bytes.
 *
 * Grows by at least the current capacity, so a run of appends costs amortized constant time. The
 * storage is not zeroed, because zeroing would touch every page of a doubled capacity the buffer
 * may never write and make it resident. The buffer only reads bytes it has written. The new storage
 * is taken in steps, the kept bytes and then the extension, and that is what keeps the capacity
 * arithmetic from overflowing: each step is checked against the arena, and both fit in one region
 * together.
 *
 * @param buffer Buffer to grow. Must not be `NULL`.
 * @param need   Number of bytes the caller is about to append. Must not be negative.
 */
static void buf_reserve(struct Buf* buffer, ptrdiff_t need) __attribute__((nonnull(1)));

/**
 * @brief Appends a formatted text whose length is already measured.
 *
 * @param buffer   Buffer to append to. Must not be `NULL`.
 * @param text_len Length of the formatted text, as `vsnprintf` measured it. Must not be negative.
 * @param format   `printf` format of the text. Must not be `NULL`.
 * @param args     Arguments that the format converts, not yet read.
 */
static void buf_write_formatted(struct Buf* buffer, int text_len, const char* format, va_list args)
    __attribute__((format(printf, 3, 0), nonnull(1, 3)));

void buf_append(struct Buf* buffer, struct Str s) {
  assert(s.len >= 0);
  buf_reserve(buffer, s.len);
  // `memcpy` requires valid pointers even for zero bytes, and an empty string may hold `NULL`.
  if (s.len > 0) {
    memcpy(buffer->data + buffer->len, s.data, (size_t)s.len);
    buffer->len += s.len;
  }
}

void buf_append_char(struct Buf* buffer, char c) {
  buf_reserve(buffer, 1);
  buffer->data[buffer->len] = c;
  buffer->len++;
}

void buf_printf(struct Buf* buffer, const char* format, ...) {
  // The first pass measures the text, and the second writes it, as in `buf_vprintf`. Starting the
  // list twice needs no `va_copy`. Passing the list to `buf_vprintf` instead makes Clang's analyzer
  // report its copy as uninitialized when it targets AArch64 Linux, whose `va_list` is a structure.
  va_list args;
  va_start(args, format);
  const int text_len = vsnprintf(NULL, 0, format, args);
  va_end(args);
  va_start(args, format);
  buf_write_formatted(buffer, text_len, format, args);
  va_end(args);
}

void buf_vprintf(struct Buf* buffer, const char* format, va_list args) {
  // The first pass measures the text, and the second writes it into room the buffer reserved.
  va_list measure_args;
  va_copy(measure_args, args);
  const int text_len = vsnprintf(NULL, 0, format, measure_args);
  va_end(measure_args);
  buf_write_formatted(buffer, text_len, format, args);
}

struct Str buf_str(const struct Buf* buffer) {
  return (struct Str){buffer->data, buffer->len};
}

char* buf_cstr(struct Buf* buffer) {
  buf_reserve(buffer, 1);
  buffer->data[buffer->len] = '\0';
  return buffer->data;
}

static void buf_reserve(struct Buf* buffer, ptrdiff_t need) {
  if (buffer->cap - buffer->len >= need) {
    return;
  }
  struct Arena* arena = buffer->arena;
  if (buffer->data == NULL || buffer->data + buffer->cap != arena->beg) {
    // An empty allocation marks where a new run of bytes starts, past any gap the arena keeps
    // between allocations. The extension then takes room for the kept bytes right there.
    char* data = arena_alloc(arena, 0, 1, 1);
    (void)arena_extend(arena, buffer->cap, 1);
    // `memcpy` requires valid pointers even for zero bytes, and `data` is `NULL` before the first
    // append.
    if (buffer->data != NULL) {
      memcpy(data, buffer->data, (size_t)buffer->len);
    }
    buffer->data = data;
  }
  // `data` now ends where the free space begins, so this extends it in place.
  const ptrdiff_t extend = buffer->cap > need ? buffer->cap : need;
  (void)arena_extend(arena, extend, 1);
  buffer->cap += extend;
}

static void buf_write_formatted(struct Buf* buffer,
                                int text_len,
                                const char* format,
                                va_list args) {
  // `vsnprintf` fails only on a wide-character conversion it cannot encode, which a byte format
  // never has, or on text longer than `INT_MAX` bytes, which no diagnostic comes near. The `int`
  // that it counts in is converted here and nowhere else.
  assert(text_len >= 0);
  // One byte more than the text, for the terminator `vsnprintf` writes. `len` does not count it.
  buf_reserve(buffer, (ptrdiff_t)text_len + 1);
  (void)vsnprintf(buffer->data + buffer->len, (size_t)text_len + 1, format, args);
  buffer->len += text_len;
}
