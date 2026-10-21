#include <acutest.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "core/parse.h"
#include "shared/str.h"

// A bare run of digits parses to its numeric value, including zero. `parse_size` documents that it
// accepts a bare run of ASCII digits, and `007` is one, so leading zeros are valid rather than an
// error. Nothing downstream reads a leading zero as an octal prefix.
static void test_parse_size_accepts_digits(void) {
  ptrdiff_t value = 12345;
  TEST_CHECK(parse_size(S("0"), &value) == 0);
  TEST_CHECK(value == 0);
  TEST_CHECK(parse_size(S("42"), &value) == 0);
  TEST_CHECK(value == 42);
  TEST_CHECK(parse_size(S("007"), &value) == 0);
  TEST_CHECK(value == 7);
}

// `PTRDIFF_MAX` itself parses, a larger value up to `SIZE_MAX` saturates to it, and one past
// `SIZE_MAX` overflows rather than wrapping. Every input is derived from the two maxima, so the
// boundary claims stay true on a 32-bit target.
static void test_parse_size_saturates_and_rejects_overflow(void) {
  char text[32];
  int text_len = snprintf(text, sizeof(text), "%td", PTRDIFF_MAX);
  TEST_ASSERT(text_len > 0 && text_len < (int)sizeof(text));
  ptrdiff_t value = 0;
  TEST_CHECK(parse_size((struct Str){text, text_len}, &value) == 0);
  TEST_CHECK(value == PTRDIFF_MAX);

  // The last digit of `PTRDIFF_MAX` is 7 on every two's-complement target, so 8 is one past it.
  text[text_len - 1] = '8';
  value = 7;
  TEST_CHECK(parse_size((struct Str){text, text_len}, &value) == 0);
  TEST_CHECK(value == PTRDIFF_MAX);

  text_len = snprintf(text, sizeof(text), "%zu", SIZE_MAX);
  TEST_ASSERT(text_len > 0 && text_len < (int)sizeof(text));
  value = 7;
  TEST_CHECK(parse_size((struct Str){text, text_len}, &value) == 0);
  TEST_CHECK(value == PTRDIFF_MAX);

  // The last digit of `SIZE_MAX` is 5 on both 32-bit and 64-bit targets, so 6 is one past it, and
  // appending a `0` to the original is a larger overflow.
  text[text_len - 1] = '6';
  value = 7;
  TEST_CHECK(parse_size((struct Str){text, text_len}, &value) == -1);
  TEST_CHECK(value == 7);
  text[text_len - 1] = '5';
  text[text_len] = '0';
  TEST_CHECK(parse_size((struct Str){text, text_len + 1}, &value) == -1);
  TEST_CHECK(value == 7);
}

// Signs, whitespace, empty input, and trailing characters are all rejected.
static void test_parse_size_rejects_malformed(void) {
  ptrdiff_t value = 7;
  TEST_CHECK(parse_size(S(""), &value) == -1);
  TEST_CHECK(parse_size(S("-1"), &value) == -1);
  TEST_CHECK(parse_size(S("+1"), &value) == -1);
  TEST_CHECK(parse_size(S(" 1"), &value) == -1);
  TEST_CHECK(parse_size(S("1 "), &value) == -1);
  TEST_CHECK(parse_size(S("12x"), &value) == -1);
  TEST_CHECK(parse_size(S("abc"), &value) == -1);
  TEST_CHECK(parse_size(S("1\0"), &value) == -1);
  // The output is left untouched on every rejection.
  TEST_CHECK(value == 7);
}

TEST_LIST = {
    {"parse size accepts digits", test_parse_size_accepts_digits},
    {"parse size saturates and rejects overflow", test_parse_size_saturates_and_rejects_overflow},
    {"parse size rejects malformed", test_parse_size_rejects_malformed},
    {NULL, NULL},
};
