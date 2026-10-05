#include <acutest.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "core/parse.h"

// A bare run of digits parses to its numeric value, including zero. `parse_size` documents that it
// accepts a bare run of ASCII digits, and `007` is one, so leading zeros are valid rather than an
// error. Nothing downstream reads a leading zero as an octal prefix.
static void test_parse_size_accepts_digits(void) {
  size_t value = 12345;
  TEST_CHECK(parse_size("0", &value) == 0);
  TEST_CHECK(value == 0);
  TEST_CHECK(parse_size("42", &value) == 0);
  TEST_CHECK(value == 42);
  TEST_CHECK(parse_size("007", &value) == 0);
  TEST_CHECK(value == 7);
}

// `SIZE_MAX` itself parses, and a digit run one digit wider overflows rather than wrapping. Both
// inputs are derived from `SIZE_MAX`, so the boundary claim stays true on a 32-bit `size_t`.
// `parse_size` documents that the largest accepted value is `SIZE_MAX` for exactly this reason.
static void test_parse_size_accepts_max_and_rejects_overflow(void) {
  char text_max[32];
  const int max_len = snprintf(text_max, sizeof(text_max), "%zu", (size_t)SIZE_MAX);
  TEST_ASSERT(max_len > 0 && (size_t)max_len < sizeof(text_max));
  size_t value = 0;
  TEST_CHECK(parse_size(text_max, &value) == 0);
  TEST_CHECK(value == SIZE_MAX);

  char text_over[32];
  const int over_len = snprintf(text_over, sizeof(text_over), "%zu0", (size_t)SIZE_MAX);
  TEST_ASSERT(over_len > 0 && (size_t)over_len < sizeof(text_over));
  // This uses a sentinel rather than the `SIZE_MAX` the successful call above left. On overflow
  // `strtoull` returns `ULLONG_MAX`, which casts to exactly `SIZE_MAX`, so `value == SIZE_MAX`
  // would also hold for an implementation that wrote the output before failing. This is the
  // untouched-on-failure claim the malformed cases make, on the one branch they cannot reach.
  value = 7;
  TEST_CHECK(parse_size(text_over, &value) == -1);
  TEST_CHECK(value == 7);
}

// Signs, whitespace, empty input, and trailing characters are all rejected.
static void test_parse_size_rejects_malformed(void) {
  size_t value = 7;
  TEST_CHECK(parse_size("", &value) == -1);
  TEST_CHECK(parse_size("-1", &value) == -1);
  TEST_CHECK(parse_size("+1", &value) == -1);
  TEST_CHECK(parse_size(" 1", &value) == -1);
  TEST_CHECK(parse_size("1 ", &value) == -1);
  TEST_CHECK(parse_size("12x", &value) == -1);
  TEST_CHECK(parse_size("abc", &value) == -1);
  // The output is left untouched on every rejection.
  TEST_CHECK(value == 7);
}

TEST_LIST = {
    {"parse size accepts digits", test_parse_size_accepts_digits},
    {"parse size accepts max and rejects overflow",
     test_parse_size_accepts_max_and_rejects_overflow},
    {"parse size rejects malformed", test_parse_size_rejects_malformed},
    {NULL, NULL},
};
