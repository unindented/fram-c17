#include "shared/string_buffer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/** First heap capacity in bytes, allocated when an empty buffer first grows. */
enum { STRING_BUFFER_CAPACITY_MIN = 256 };

void string_buffer_init(struct StringBuffer* buffer) {
  *buffer = (struct StringBuffer){0};
}

void string_buffer_free(struct StringBuffer* buffer) {
  free(buffer->data);
  string_buffer_init(buffer);
}

int string_buffer_reserve(struct StringBuffer* buffer, size_t extra_len) {
  // The guard runs before the addition below and uses `>=` rather than `>`, because `needed` adds
  // one more byte for the terminator. `extra_len == SIZE_MAX - len` already makes
  // `len + extra_len + 1` wrap. The wraparound is itself defined, but a wrapped `needed` would
  // leave `capacity` unchanged, and the `memcpy` in `string_buffer_append_len` would then write
  // past the allocation.
  if (extra_len >= SIZE_MAX - buffer->len) {
    return -1;
  }
  const size_t needed = buffer->len + extra_len + 1;
  if (needed > buffer->capacity) {
    size_t next = buffer->capacity == 0 ? STRING_BUFFER_CAPACITY_MIN : buffer->capacity;
    while (next < needed) {
      // Doubling stops at `SIZE_MAX / 2` because one more doubling past that overflows `size_t`.
      // Past the guard, the target becomes `needed` exactly. This container already receives the
      // byte total it must reach, and the code above proved `needed` does not wrap, so an enormous
      // but satisfiable request can still succeed instead of being refused.
      if (next > SIZE_MAX / 2) {
        next = needed;
        break;
      }
      next *= 2;
    }
    // A temporary preserves the old allocation when `realloc` fails.
    char* data = realloc(buffer->data, next);
    if (data == NULL) {
      return -1;
    }
    buffer->capacity = next;
    buffer->data = data;
  }
  buffer->data[buffer->len] = '\0';
  return 0;
}

int string_buffer_append_len(struct StringBuffer* buffer, const char* str, size_t str_len) {
  if (string_buffer_reserve(buffer, str_len) != 0) {
    return -1;
  }
  // The guard is for `str`, not for speed. `memcpy` requires a valid pointer even for a zero-byte
  // copy, and the contract lets `str` be `NULL` when `str_len` is 0.
  if (str_len > 0) {
    memcpy(buffer->data + buffer->len, str, str_len);
    buffer->len += str_len;
    buffer->data[buffer->len] = '\0';
  }
  return 0;
}

int string_buffer_append(struct StringBuffer* buffer, const char* str) {
  return string_buffer_append_len(buffer, str, strlen(str));
}

int string_buffer_append_char(struct StringBuffer* buffer, char c) {
  return string_buffer_append_len(buffer, &c, 1);
}

char* string_buffer_steal(struct StringBuffer* buffer) {
  // A buffer that never grew still owes the caller a terminated string. It is already reset.
  if (buffer->data == NULL) {
    return calloc(1, 1);
  }
  char* data = buffer->data;
  string_buffer_init(buffer);
  return data;
}
