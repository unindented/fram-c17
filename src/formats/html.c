#include "formats/html.h"

#include "shared/string_buffer.h"

/**
 * @brief Returns the character reference that escapes `c`, or `NULL` when `c` needs no escaping.
 *
 * This is the only place the escape set is defined.
 *
 * @param c Byte to escape.
 * @return Terminated character reference, or `NULL` when `c` may be copied verbatim.
 */
static const char* escape_entity(char c);

int html_escape_append_len(struct StringBuffer* buffer, const char* text, size_t text_len) {
  // Ordinary bytes go into the buffer in runs rather than one at a time. The escape set is sparse
  // in real content, so scanning ahead to the next byte that needs an entity turns one append per
  // byte into one append per entity plus one per run between them.
  size_t run_start = 0;
  for (size_t i = 0; i < text_len; i++) {
    const char* replacement = escape_entity(text[i]);
    if (replacement == NULL) {
      continue;
    }
    // The non-aliasing precondition in `formats/html.h` is necessary because this forwards
    // `text + run_start` into the destination unchanged. A caller escaping a buffer into itself
    // hands `string_buffer_append_len` a source inside its own destination, and the growth frees
    // that region before the copy.
    if (i > run_start && string_buffer_append_len(buffer, text + run_start, i - run_start) != 0) {
      return -1;
    }
    if (string_buffer_append(buffer, replacement) != 0) {
      return -1;
    }
    run_start = i + 1;
  }
  if (text_len > run_start) {
    return string_buffer_append_len(buffer, text + run_start, text_len - run_start);
  }
  return 0;
}

static const char* escape_entity(char c) {
  switch (c) {
    case '&':
      return "&amp;";
    case '<':
      return "&lt;";
    case '>':
      return "&gt;";
    case '"':
      return "&quot;";
    case '\'':
      // A numeric reference, not `&apos;`, because that named entity is undefined in HTML 4 and
      // plain XML. `&#39;` is the portable choice across this project's HTML and XML output.
      return "&#39;";
    default:
      return NULL;
  }
}
