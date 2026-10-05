#include <acutest.h>
#include <stdlib.h>
#include <string.h>

#include "core/text.h"
#include "shared/arena.h"

/**
 * @brief Slugifies a whole terminated string.
 *
 * Discards the reported length; `test_slugify_reports_length` covers it separately.
 *
 * @param text  Text to slugify.
 * @param arena Arena that owns the returned string.
 * @return The arena-owned slug, or `NULL` on failure.
 */
static char* slugify(const char* text, struct Arena* arena) {
  size_t slug_len = 0;
  return text_slugify(text, strlen(text), arena, &slug_len);
}

// Duplicating a string yields a heap copy independent of its source.
static void test_strdup_yields_independent_copy(void) {
  const char* source = "public/index.html";
  char* copy = text_strdup(source);
  TEST_ASSERT(copy != NULL);
  TEST_CHECK(copy != source);
  TEST_CHECK(strcmp(copy, source) == 0);
  free(copy);

  char* empty = text_strdup("");
  TEST_ASSERT(empty != NULL);
  TEST_CHECK(strcmp(empty, "") == 0);
  free(empty);
}

// `text_is_nul_free` accepts arbitrary nonzero bytes and the empty buffer, but rejects a zero at
// every position rather than relying on C-string termination.
static void test_nul_free_checks_bounded_bytes(void) {
  static const unsigned char clean[] = {'a', 0xFF, 'z'};
  static const unsigned char leading[] = {'\0', 'a'};
  static const unsigned char trailing[] = {'a', '\0'};

  TEST_CHECK(text_is_nul_free(NULL, 0));
  TEST_CHECK(text_is_nul_free(clean, sizeof(clean)));
  TEST_CHECK(!text_is_nul_free(leading, sizeof(leading)));
  TEST_CHECK(!text_is_nul_free(trailing, sizeof(trailing)));
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
  TEST_CHECK(text_is_safe_identifier("partial_layout-1"));
  TEST_CHECK(!text_is_safe_identifier("a/b"));
  TEST_CHECK(!text_is_safe_identifier("a.b"));
  TEST_CHECK(!text_is_safe_identifier("a b"));
  TEST_CHECK(!text_is_safe_identifier("caf\xC3\xA9"));
  TEST_CHECK(!text_is_safe_identifier("../partial"));
  TEST_CHECK(!text_is_safe_identifier(""));
  TEST_CHECK(!text_is_safe_identifier(NULL));
}

// A run of ASCII punctuation, underscores, and spaces collapses to one dash and uppercase folds
// down, so `Hello, C17_World!` yields a single dash between words rather than one per stripped
// byte. Text with nothing usable in it takes `SLUG_FALLBACK` instead of yielding an empty slug.
static void test_slugify_normalizes_and_defaults(void) {
  struct Arena arena;
  arena_init(&arena);
  char* slug = slugify("Hello, C17_World!", &arena);
  TEST_ASSERT(slug != NULL);
  TEST_CHECK(strcmp(slug, "hello-c17-world") == 0);
  TEST_CHECK(strcmp(slugify("!!!", &arena), "untitled") == 0);
  arena_free(&arena);
}

// Non-ASCII bytes fold to their lowercase hex value rather than being dropped, so two names that
// differ only outside ASCII produce different slugs instead of both collapsing to the fallback and
// claiming the same output path.
static void test_slugify_folds_non_ascii_distinctly(void) {
  struct Arena arena;
  arena_init(&arena);

  // Each expected slug is the input's UTF-8 bytes in hex: `日本語` is E6 97 A5 E6 9C AC E8 AA 9E,
  // and `中文` is E4 B8 AD E6 96 87.
  TEST_CHECK(strcmp(slugify("日本語", &arena), "e697a5e69cace8aa9e") == 0);
  TEST_CHECK(strcmp(slugify("中文", &arena), "e4b8ade69687") == 0);

  // ASCII text is unaffected by the fold, and a mixed name keeps its ASCII part readable. `é` is
  // C3 A9.
  TEST_CHECK(strcmp(slugify("Plain Title", &arena), "plain-title") == 0);
  TEST_CHECK(strcmp(slugify("Café", &arena), "cafc3a9") == 0);

  // A run of separators still collapses to one dash around a folded byte. The em dash is E2 80 94.
  TEST_CHECK(strcmp(slugify("a — b", &arena), "a-e28094-b") == 0);

  arena_free(&arena);
}

// Only the requested prefix is slugified, so a caller can slugify one segment of a path in place
// instead of copying it out to get a terminated string first.
static void test_slugify_honors_length(void) {
  struct Arena arena;
  arena_init(&arena);

  const char* path = "Summer_Trip/Beach/sunset.jpg";
  size_t slug_len = 0;
  TEST_CHECK(strcmp(text_slugify(path, strlen("Summer_Trip"), &arena, &slug_len), "summer-trip") ==
             0);
  TEST_CHECK(strcmp(text_slugify(path + 12, strlen("Beach"), &arena, &slug_len), "beach") == 0);
  // A zero-length slice has no usable characters, so it takes the fallback.
  TEST_CHECK(strcmp(text_slugify(path, 0, &arena, &slug_len), "untitled") == 0);

  arena_free(&arena);
}

// The reported length matches the slug, on the folding path where it is not the input's length and
// on the fallback path that copies `SLUG_FALLBACK`. A caller that sizes a buffer from this rather
// than rescanning the slug would under-allocate on a wrong value, so it is not a cosmetic error.
static void test_slugify_reports_length(void) {
  struct Arena arena;
  arena_init(&arena);

  size_t slug_len = 0;
  const char* slug = text_slugify("Plain Title", strlen("Plain Title"), &arena, &slug_len);
  TEST_CHECK(strcmp(slug, "plain-title") == 0);
  TEST_CHECK(slug_len == strlen(slug));

  // A folded byte makes the slug longer than its source, so the length is not the input's.
  slug = text_slugify("Café", strlen("Café"), &arena, &slug_len);
  TEST_CHECK(strcmp(slug, "cafc3a9") == 0);
  TEST_CHECK(slug_len == strlen(slug));
  TEST_CHECK(slug_len > strlen("Café"));

  // The fallback returns a copy of the literal rather than the buffer the loop fills, so its length
  // is written by a separate statement.
  slug = text_slugify("---", strlen("---"), &arena, &slug_len);
  TEST_CHECK(strcmp(slug, "untitled") == 0);
  TEST_CHECK(slug_len == strlen("untitled"));

  arena_free(&arena);
}

TEST_LIST = {
    {"strdup yields independent copy", test_strdup_yields_independent_copy},
    {"nul free checks bounded bytes", test_nul_free_checks_bounded_bytes},
    {"safe identifier accepts narrow alphabet", test_safe_identifier_accepts_narrow_alphabet},
    {"slugify normalizes and defaults", test_slugify_normalizes_and_defaults},
    {"slugify folds non-ascii distinctly", test_slugify_folds_non_ascii_distinctly},
    {"slugify honors length", test_slugify_honors_length},
    {"slugify reports length", test_slugify_reports_length},
    {NULL, NULL},
};
