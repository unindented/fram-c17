#include "core/text.h"

#include <stddef.h>
#include <string.h>

#include "core/ascii.h"
#include "shared/arena.h"

/** Slug used when the source text yields no alphanumeric characters. */
#define SLUG_FALLBACK S("untitled")

/** Lowercase hex digits used to fold a non-ASCII byte into URL-safe output. */
static const char HEX_DIGITS[] = "0123456789abcdef";

bool text_is_nul_free(struct Str text) {
  return text.len == 0 || memchr(text.data, '\0', (size_t)text.len) == NULL;
}

bool text_is_safe_identifier(struct Str name) {
  if (name.len == 0) {
    return false;
  }
  for (ptrdiff_t i = 0; i < name.len; i++) {
    const unsigned char c = (unsigned char)name.data[i];
    if (!ascii_is_alphanumeric(c) && c != '_' && c != '-') {
      return false;
    }
  }
  return true;
}

struct Str text_slugify(struct Str text, struct Arena* arena) {
  // This decides the fallback before allocating. A text with no alphanumeric and no non-ASCII byte
  // slugifies to nothing, and taking the fallback after allocating the `2n` buffer would abandon
  // it. An arena never frees, so a long name that slugifies to nothing would cost twice its length
  // for the rest of the build.
  bool has_slug_byte = false;
  for (ptrdiff_t i = 0; i < text.len && !has_slug_byte; i++) {
    const unsigned char c = (unsigned char)text.data[i];
    has_slug_byte = ascii_is_alphanumeric(c) || c >= 0x80;
  }
  if (!has_slug_byte) {
    return SLUG_FALLBACK;
  }

  // A non-ASCII byte expands to two hex digits. Every other byte yields at most one output byte. A
  // buffer twice the input length always holds the slug. Trimming a trailing dash can only shorten
  // it. One arena buffer sized up front can hold it in place. It is requested as `text.len`
  // two-byte units, so the arena's own bounds check covers the doubling.
  char* slug = arena_alloc(arena, text.len, 2, 1);
  ptrdiff_t len = 0;
  bool is_after_dash = true;
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const unsigned char c = (unsigned char)text.data[i];
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
  return (struct Str){slug, len};
}
