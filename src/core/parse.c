#include "core/parse.h"

#include <stddef.h>
#include <stdint.h>

#include "core/ascii.h"
#include "shared/str.h"

int parse_size(struct Str text, ptrdiff_t* value_out) {
  if (text.len == 0) {
    return -1;
  }
  size_t value = 0;
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const unsigned char c = (unsigned char)text.data[i];
    if (!ascii_is_digit(c)) {
      return -1;
    }
    const size_t digit = (size_t)(c - '0');
    // Comparing against the quotient keeps the check itself from overflowing.
    if (value > (SIZE_MAX - digit) / 10) {
      return -1;
    }
    value = value * 10 + digit;
  }
  // A count this large is still a number: saturating lets a caller with an upper limit report that
  // limit rather than calling it not a number.
  *value_out = value > (size_t)PTRDIFF_MAX ? PTRDIFF_MAX : (ptrdiff_t)value;
  return 0;
}
