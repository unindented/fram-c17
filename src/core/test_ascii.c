#include <acutest.h>

#include "core/ascii.h"

// `ascii_is_digit` answers for `[0-9]` and nothing else, asserted at both edges. Only `parse_size`
// re-checks the byte afterwards by handing it to `strtoull`. `parse_duration_ms` in `video.c` does
// not: widening the set would let it accumulate a non-digit byte of `ffprobe` output as a digit.
static void test_is_digit_accepts_only_ascii_digits(void) {
  TEST_CHECK(ascii_is_digit('0'));
  TEST_CHECK(ascii_is_digit('5'));
  TEST_CHECK(ascii_is_digit('9'));

  // The bytes immediately outside the range on each side.
  TEST_CHECK(!ascii_is_digit('/'));
  TEST_CHECK(!ascii_is_digit(':'));
  TEST_CHECK(!ascii_is_digit('a'));
  TEST_CHECK(!ascii_is_digit(' '));
  TEST_CHECK(!ascii_is_digit('\0'));
  TEST_CHECK(!ascii_is_digit(0xC3));
}

// `ascii_is_alphanumeric` answers for `[0-9A-Za-z]` and nothing else, asserted at every edge of the
// three ranges. This predicate defines the alphanumeric accept set of `text_is_safe_identifier`,
// `path_is_safe_relative` and `is_derivative_name`, the pass-through set of `text_slugify`, and the
// unencoded set of `is_url_literal` in `media_item.c`, so widening it by one byte silently changes
// which identifiers, output paths and derivative names are accepted, which slugs a build generates,
// and which bytes a media URL leaves unencoded. An album directory named `Album_Name` publishes
// under the slug `album-name`. An added `'_'` would change that to `album_name`.
static void test_is_alnum_accepts_only_letters_and_digits(void) {
  TEST_CHECK(ascii_is_alphanumeric('0'));
  TEST_CHECK(ascii_is_alphanumeric('9'));
  TEST_CHECK(ascii_is_alphanumeric('A'));
  TEST_CHECK(ascii_is_alphanumeric('Z'));
  TEST_CHECK(ascii_is_alphanumeric('a'));
  TEST_CHECK(ascii_is_alphanumeric('z'));

  // The byte on each side of each range: '/'/':' bracket the digits, '@'/'[' the uppercase letters,
  // '`'/'{' the lowercase ones.
  TEST_CHECK(!ascii_is_alphanumeric('/'));
  TEST_CHECK(!ascii_is_alphanumeric(':'));
  TEST_CHECK(!ascii_is_alphanumeric('@'));
  TEST_CHECK(!ascii_is_alphanumeric('['));
  TEST_CHECK(!ascii_is_alphanumeric('`'));
  TEST_CHECK(!ascii_is_alphanumeric('{'));

  // The punctuation the path and identifier rules allow separately, which this predicate must not
  // fold in itself, plus the ends of the byte range.
  TEST_CHECK(!ascii_is_alphanumeric('_'));
  TEST_CHECK(!ascii_is_alphanumeric('-'));
  TEST_CHECK(!ascii_is_alphanumeric('.'));
  TEST_CHECK(!ascii_is_alphanumeric(' '));
  TEST_CHECK(!ascii_is_alphanumeric('\0'));
  TEST_CHECK(!ascii_is_alphanumeric(0x7F));
  TEST_CHECK(!ascii_is_alphanumeric(0xC3));
  TEST_CHECK(!ascii_is_alphanumeric(0xFF));
}

// `ascii_is_space` answers for the six bytes `isspace` accepts in the C locale and nothing else,
// asserted at both edges of the `\t` through `\r` run. Its only caller, `parse_duration_ms` in
// `video.c`, uses it to trim the duration `ffprobe` prints.
static void test_is_space_accepts_only_ascii_whitespace(void) {
  TEST_CHECK(ascii_is_space(' '));
  TEST_CHECK(ascii_is_space('\t'));
  TEST_CHECK(ascii_is_space('\n'));
  TEST_CHECK(ascii_is_space('\v'));
  TEST_CHECK(ascii_is_space('\f'));
  TEST_CHECK(ascii_is_space('\r'));

  // The bytes immediately outside the control run and around the space, plus bytes that some
  // locales treat as whitespace.
  TEST_CHECK(!ascii_is_space(0x08));
  TEST_CHECK(!ascii_is_space(0x0E));
  TEST_CHECK(!ascii_is_space(0x1F));
  TEST_CHECK(!ascii_is_space('!'));
  TEST_CHECK(!ascii_is_space('\0'));
  TEST_CHECK(!ascii_is_space(0x85));
  TEST_CHECK(!ascii_is_space(0xA0));
}

// `ascii_to_lower` folds `[A-Z]` and returns every other byte unchanged. Slugs, URL schemes, file
// suffixes, and manifest keys all use this exact locale-independent fold, through `text_slugify`,
// `skip_scheme`, `media_item_has_extension` and `has_suffix` in `fs.c`, and `manifest_equal_bytes`
// with `manifest_hash_bytes`. `skip_scheme` folds both the URL byte and the scheme byte before
// comparing, and a fold that set bit 0x20 unconditionally would make the control byte 0x1A compare
// equal to ':' and 0x0F equal to '/'. A scheme-less `base_url` spelled with those control bytes
// would then pass as absolute. The high-byte case pins the rule that bytes outside ASCII never
// fold, whatever the locale: `tolower` from `<ctype.h>` may fold such a byte under some locales,
// and this must not.
static void test_to_lower_folds_uppercase_and_passes_through(void) {
  TEST_CHECK(ascii_to_lower('A') == 'a');
  TEST_CHECK(ascii_to_lower('Z') == 'z');
  TEST_CHECK(ascii_to_lower('M') == 'm');

  // Already lowercase, and the bytes immediately outside `[A-Z]`.
  TEST_CHECK(ascii_to_lower('a') == 'a');
  TEST_CHECK(ascii_to_lower('z') == 'z');
  TEST_CHECK(ascii_to_lower('@') == '@');
  TEST_CHECK(ascii_to_lower('[') == '[');

  // Digits, punctuation and the two control bytes that would alias ':' and '/' under an
  // unconditional fold.
  TEST_CHECK(ascii_to_lower('7') == '7');
  TEST_CHECK(ascii_to_lower(':') == ':');
  TEST_CHECK(ascii_to_lower('/') == '/');
  TEST_CHECK(ascii_to_lower(0x1A) == 0x1A);
  TEST_CHECK(ascii_to_lower(0x0F) == 0x0F);
  TEST_CHECK(ascii_to_lower('\0') == '\0');

  // Above 0x7F nothing is folded, whatever the locale.
  TEST_CHECK(ascii_to_lower(0xC3) == 0xC3);
  TEST_CHECK(ascii_to_lower(0xDF) == 0xDF);
  TEST_CHECK(ascii_to_lower(0xFF) == 0xFF);
}

TEST_LIST = {
    {"is digit accepts only ascii digits", test_is_digit_accepts_only_ascii_digits},
    {"is alnum accepts only letters and digits", test_is_alnum_accepts_only_letters_and_digits},
    {"is space accepts only ascii whitespace", test_is_space_accepts_only_ascii_whitespace},
    {"to lower folds uppercase and passes through",
     test_to_lower_folds_uppercase_and_passes_through},
    {NULL, NULL},
};
