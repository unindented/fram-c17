#include "formats/html.h"

#include <stddef.h>

#include "shared/buf.h"
#include "shared/str.h"

/**
 * @brief Returns the character reference that escapes `c`, or an empty string when `c` needs no
 *        escaping.
 *
 * This is the only place the escape set is defined.
 *
 * @param c Byte to escape.
 * @return Character reference, or an empty string when `c` may be copied verbatim.
 */
static struct Str escape_entity(char c);

void html_escape_append(struct Buf* buffer, struct Str text) {
  // Ordinary bytes go into the buffer in runs rather than one at a time. The escape set is sparse
  // in real content, so scanning ahead to the next byte that needs an entity turns one append per
  // byte into one append per entity plus one per run between them.
  ptrdiff_t run_start = 0;
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const struct Str replacement = escape_entity(text.data[i]);
    if (replacement.len == 0) {
      continue;
    }
    buf_append(buffer, str_head(str_tail(text, run_start), i - run_start));
    buf_append(buffer, replacement);
    run_start = i + 1;
  }
  buf_append(buffer, str_tail(text, run_start));
}

static struct Str escape_entity(char c) {
  switch (c) {
    case '&':
      return S("&amp;");
    case '<':
      return S("&lt;");
    case '>':
      return S("&gt;");
    case '"':
      return S("&quot;");
    case '\'':
      // A numeric reference, not `&apos;`, because that named entity is undefined in HTML 4 and
      // plain XML. `&#39;` is the portable choice across this project's HTML and XML output.
      return S("&#39;");
    default:
      return S("");
  }
}
