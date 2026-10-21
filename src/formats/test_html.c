#include <acutest.h>
#include <stddef.h>

#include "formats/html.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"
#include "test_support.h"

// HTML escaping replaces the five special characters `&`, `<`, `>`, `"` and `'`.
static void test_html_escape_replaces_special_chars(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  html_escape_append(&buf, S("abc&<>\"'def"));
  TEST_CHECK(str_eq(buf_str(&buf), S("abc&amp;&lt;&gt;&quot;&#39;def")));
}

// Text with nothing to escape is copied through unchanged. This is the case the run batching exists
// for: the whole input is one run, appended once.
static void test_html_escape_copies_plain_text(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  html_escape_append(&buf, S("plain text, no entities here"));
  TEST_CHECK(str_eq(buf_str(&buf), S("plain text, no entities here")));
}

// Every byte outside the five-entity set is copied verbatim, including bytes above 0x7F and control
// bytes XML forbids. The golden fixtures are not relied on to feed this function a non-ASCII byte,
// so this test is what checks the signedness of the `char` read by the escape switch. A plausible
// rewrite of that switch into a 256-entry table indexed by the byte would read out of bounds on the
// first accented character in an interpolated value.
static void test_html_escape_copies_high_and_control_bytes_verbatim(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  html_escape_append(&buf, S("caf\xC3\xA9 &\x01"));
  TEST_CHECK(str_eq(buf_str(&buf), S("caf\xC3\xA9 &amp;\x01")));
}

// Special characters at the range boundaries and adjacent to each other escape with no run between
// them, which is where an off-by-one in the run bookkeeping would drop or duplicate bytes.
static void test_html_escape_handles_run_boundaries(void) {
  static const struct Str cases[][2] = {
      {STR_INIT("&x"), STR_INIT("&amp;x")},
      {STR_INIT("x&"), STR_INIT("x&amp;")},
      {STR_INIT("&&"), STR_INIT("&amp;&amp;")},
      {STR_INIT("&"), STR_INIT("&amp;")},
      {STR_INIT("<a>&<b>"), STR_INIT("&lt;a&gt;&amp;&lt;b&gt;")},
      {STR_INIT("a&&b"), STR_INIT("a&amp;&amp;b")},
  };

  struct Arena arena = test_arena();
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Buf buf = {.arena = &arena};
    html_escape_append(&buf, cases[i][0]);
    TEST_CHECK(str_eq(buf_str(&buf), cases[i][1]));
  }
}

// Escaping appends to whatever the buffer already holds rather than replacing it.
static void test_html_escape_appends_to_existing_content(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  buf_append(&buf, S("<pre>"));
  html_escape_append(&buf, S("a&b"));
  TEST_CHECK(str_eq(buf_str(&buf), S("<pre>a&amp;b")));
}

// Only the string's own bytes are escaped, so a borrowed slice does not read past its length.
static void test_html_escape_honors_length(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  html_escape_append(&buf, str_head(S("a&b<c"), 3));
  TEST_CHECK(str_eq(buf_str(&buf), S("a&amp;b")));
}

// Escaping an empty range is a no-op that leaves the buffer untouched.
static void test_html_escape_empty_input(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  html_escape_append(&buf, S(""));
  TEST_CHECK(buf.len == 0);
  TEST_CHECK(buf.data == NULL);
  // An empty string may hold `NULL`, which is a different pointer shape from the empty literal
  // above.
  html_escape_append(&buf, (struct Str){NULL, 0});
  TEST_CHECK(buf.len == 0);
  TEST_CHECK(buf.data == NULL);
}

TEST_LIST = {
    {"html escape replaces special chars", test_html_escape_replaces_special_chars},
    {"html escape copies plain text", test_html_escape_copies_plain_text},
    {"html escape copies high and control bytes verbatim",
     test_html_escape_copies_high_and_control_bytes_verbatim},
    {"html escape handles run boundaries", test_html_escape_handles_run_boundaries},
    {"html escape appends to existing content", test_html_escape_appends_to_existing_content},
    {"html escape honors length", test_html_escape_honors_length},
    {"html escape empty input", test_html_escape_empty_input},
    {NULL, NULL},
};
