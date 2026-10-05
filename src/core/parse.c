#include "core/parse.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "core/ascii.h"

int parse_size(const char* text, size_t* value_out) {
  // `strtoull` silently accepts a leading sign and whitespace, so gate on a digit first. The gate
  // is ASCII-only by construction, like every other classification in this codebase, rather than
  // locale-dependent like `<ctype.h>`. Those functions also take an `int` that must be an
  // `unsigned char` value, so a plain `char` above 0x7F would be undefined behavior.
  if (!ascii_is_digit((unsigned char)text[0])) {
    return -1;
  }
  // `strtoull` sets `errno` on overflow but never clears it, and its overflow return (`ULLONG_MAX`)
  // is also a legal parse result, so zeroing first is the only way to tell the two apart.
  errno = 0;
  char* end;
  const unsigned long long value = strtoull(text, &end, 10);
  // The digit gate above guarantees `strtoull` consumed at least one character, so `end != text`
  // always. Only overflow (`errno`) or trailing junk (`*end`) can fail here.
  if (errno != 0 || *end != '\0') {
    return -1;
  }
  // `strtoull` only reports overflow past `ULLONG_MAX`, so where `size_t` is narrower a value
  // between the two maxima parses cleanly and the cast below would truncate it silently. The `#if`
  // omits the check where the two types have the same width, because there the comparison is a
  // tautology rather than a bound.
#if SIZE_MAX < ULLONG_MAX
  if (value > SIZE_MAX) {
    return -1;
  }
#endif
  *value_out = (size_t)value;
  return 0;
}
