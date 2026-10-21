#include <acutest.h>
#include <stddef.h>

#include "core/text.h"
#include "shared/arena.h"
#include "test_support.h"

// `text_is_nul_free` accepts arbitrary nonzero bytes and the empty string, but rejects a zero at
// every position, including the last.
static void test_nul_free_checks_bounded_bytes(void) {
  TEST_CHECK(text_is_nul_free((struct Str){NULL, 0}));
  TEST_CHECK(text_is_nul_free(S("a\xFFz")));
  TEST_CHECK(!text_is_nul_free(S("\0a")));
  TEST_CHECK(!text_is_nul_free(S("a\0")));
}

// `text_is_safe_identifier` accepts only the narrow alphabet used for template partial names, its
// one caller. Each disallowed class gets its own assertion rather than one input carrying several.
// A single `"../partial"` passes as long as *either* `.` or `/` is rejected, so it cannot say which
// rule fired. A regression admitting just one of them would keep it green. This predicate is the
// only guard on a partial name before the template engine composes it into
// `<templates_dir>/partials/<name>.html`, and mustache4c's own tag validation rejects whitespace
// and `..` but not `/`. The high byte pins the locale-independence guarantee `core/ascii.h` exists
// for.
static void test_safe_identifier_accepts_narrow_alphabet(void) {
  TEST_CHECK(text_is_safe_identifier(S("partial_layout-1")));
  TEST_CHECK(!text_is_safe_identifier(S("a/b")));
  TEST_CHECK(!text_is_safe_identifier(S("a.b")));
  TEST_CHECK(!text_is_safe_identifier(S("a b")));
  TEST_CHECK(!text_is_safe_identifier(S("caf\xC3\xA9")));
  TEST_CHECK(!text_is_safe_identifier(S("../partial")));
  TEST_CHECK(!text_is_safe_identifier(S("")));
  TEST_CHECK(!text_is_safe_identifier(S("a\0b")));
}

// A run of ASCII punctuation, underscores, and spaces collapses to one dash and uppercase folds
// down, so `Hello, C17_World!` yields a single dash between words rather than one per stripped
// byte. Text with nothing usable in it takes `SLUG_FALLBACK` instead of yielding an empty slug.
static void test_slugify_normalizes_and_defaults(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(text_slugify(S("Hello, C17_World!"), &arena), S("hello-c17-world")));
  TEST_CHECK(str_eq(text_slugify(S("!!!"), &arena), S("untitled")));
}

// Non-ASCII bytes fold to their lowercase hex value rather than being dropped, so two names that
// differ only outside ASCII produce different slugs instead of both collapsing to the fallback and
// claiming the same output path.
static void test_slugify_folds_non_ascii_distinctly(void) {
  struct Arena arena = test_arena();

  // Each expected slug is the input's UTF-8 bytes in hex: `日本語` is E6 97 A5 E6 9C AC E8 AA 9E,
  // and `中文` is E4 B8 AD E6 96 87.
  TEST_CHECK(str_eq(text_slugify(S("日本語"), &arena), S("e697a5e69cace8aa9e")));
  TEST_CHECK(str_eq(text_slugify(S("中文"), &arena), S("e4b8ade69687")));

  // ASCII text is unaffected by the fold, and a mixed name keeps its ASCII part readable. `é` is
  // C3 A9.
  TEST_CHECK(str_eq(text_slugify(S("Plain Title"), &arena), S("plain-title")));
  TEST_CHECK(str_eq(text_slugify(S("Café"), &arena), S("cafc3a9")));

  // A run of separators still collapses to one dash around a folded byte. The em dash is E2 80 94.
  TEST_CHECK(str_eq(text_slugify(S("a — b"), &arena), S("a-e28094-b")));
}

// Only the given bytes are slugified, so a caller can slugify one segment of a path in place.
static void test_slugify_honors_length(void) {
  struct Arena arena = test_arena();

  const struct Str path = S("Summer_Trip/Beach/sunset.jpg");
  TEST_CHECK(str_eq(text_slugify(str_head(path, 11), &arena), S("summer-trip")));
  TEST_CHECK(str_eq(text_slugify(str_head(str_tail(path, 12), 5), &arena), S("beach")));
  // A zero-length slice has no usable characters, so it takes the fallback.
  TEST_CHECK(str_eq(text_slugify(str_head(path, 0), &arena), S("untitled")));
}

// A `NUL` byte is an ordinary separator byte, so it collapses to a dash like any other punctuation
// rather than ending the text.
static void test_slugify_treats_nul_as_separator(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(text_slugify(S("a\0b"), &arena), S("a-b")));
}

TEST_LIST = {
    {"nul free checks bounded bytes", test_nul_free_checks_bounded_bytes},
    {"safe identifier accepts narrow alphabet", test_safe_identifier_accepts_narrow_alphabet},
    {"slugify normalizes and defaults", test_slugify_normalizes_and_defaults},
    {"slugify folds non-ascii distinctly", test_slugify_folds_non_ascii_distinctly},
    {"slugify honors length", test_slugify_honors_length},
    {"slugify treats nul as separator", test_slugify_treats_nul_as_separator},
    {NULL, NULL},
};
