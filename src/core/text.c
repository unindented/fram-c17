#include "core/text.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/ascii.h"
#include "shared/arena.h"

/** Slug used when the source text yields no alphanumeric characters. */
static const char SLUG_FALLBACK[] = "untitled";

/** Lowercase hex digits used to fold a non-ASCII byte into URL-safe output. */
static const char HEX_DIGITS[] = "0123456789abcdef";

char* text_strdup(const char* text) {
  const size_t text_len = strlen(text);
  char* copy = malloc(text_len + 1);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, text, text_len + 1);
  return copy;
}

bool text_is_nul_free(const unsigned char* data, size_t data_len) {
  return data_len == 0 || memchr(data, '\0', data_len) == NULL;
}

bool text_is_safe_identifier(const char* name) {
  if (name == NULL || *name == '\0') {
    return false;
  }
  for (const unsigned char* p = (const unsigned char*)name; *p != '\0'; p++) {
    if (!ascii_is_alphanumeric(*p) && *p != '_' && *p != '-') {
      return false;
    }
  }
  return true;
}

char* text_slugify(const char* text, size_t text_len, struct Arena* arena, size_t* slug_len_out) {
  // This decides the fallback before allocating. A text with no alphanumeric and no non-ASCII byte
  // slugifies to nothing, and taking the fallback after allocating the `2n + 1` buffer would
  // abandon it. An arena never frees, so a long name that slugifies to nothing would cost twice its
  // length for the rest of the build.
  bool has_slug_byte = false;
  for (size_t i = 0; i < text_len && !has_slug_byte; i++) {
    const unsigned char c = (unsigned char)text[i];
    has_slug_byte = ascii_is_alphanumeric(c) || c >= 0x80;
  }
  if (!has_slug_byte) {
    char* fallback = arena_strdup(arena, SLUG_FALLBACK);
    if (fallback == NULL) {
      return NULL;
    }
    *slug_len_out = sizeof(SLUG_FALLBACK) - 1;
    return fallback;
  }

  // A non-ASCII byte expands to two hex digits. Every other byte yields at most one output byte. A
  // buffer twice the input length always holds the slug. Trimming a trailing dash can only shorten
  // it. One arena buffer sized up front can hold it in place. The check runs before the computation
  // because `size_t` wraps rather than trapping. A `text_len` above `(SIZE_MAX - 1) / 2` would wrap
  // `text_len * 2 + 1` to a small value. The loop below would then write past the buffer that size
  // allocated.
  if (text_len > (SIZE_MAX - 1) / 2) {
    return NULL;
  }
  char* slug = arena_alloc(arena, text_len * 2 + 1);
  if (slug == NULL) {
    return NULL;
  }
  size_t len = 0;
  bool is_after_dash = true;
  for (size_t i = 0; i < text_len; i++) {
    const unsigned char c = (unsigned char)text[i];
    if (ascii_is_alphanumeric(c)) {
      slug[len++] = (char)ascii_to_lower(c);
      is_after_dash = false;
    } else if (c >= 0x80) {
      // Fold a non-ASCII byte to its hex value instead of treating it as a separator. Dropping it
      // would collapse every non-Latin name to `SLUG_FALLBACK`, so two such names would claim the
      // same output path and fail the build as duplicates. Hex keeps them distinct and URL-safe.
      slug[len++] = HEX_DIGITS[c >> 4];
      slug[len++] = HEX_DIGITS[c & 0x0F];
      is_after_dash = false;
    } else if (!is_after_dash) {
      // Collapse a run of separators into the single dash emitted on the first one.
      slug[len++] = '-';
      is_after_dash = true;
    }
  }
  if (slug[len - 1] == '-') {
    len--;
  }
  // There is no empty-result check, because the pre-scan above guarantees at least one alphanumeric
  // or non-ASCII byte, which writes at least one character and clears `is_after_dash`. The loop
  // only ever writes a dash after that, so position 0 never holds one and the trim cannot empty the
  // slug.
  slug[len] = '\0';
  *slug_len_out = len;
  return slug;
}
