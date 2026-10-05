#include <acutest.h>
#include <string.h>

#include "formats/html.h"
#include "shared/string_buffer.h"

// HTML escaping replaces the five special characters `&`, `<`, `>`, `"` and `'`.
static void test_html_escape_replaces_special_chars(void) {
  struct StringBuffer buf;
  string_buffer_init(&buf);
  const char* input = "abc&<>\"'def";
  TEST_CHECK(html_escape_append_len(&buf, input, strlen(input)) == 0);
  TEST_CHECK(strcmp(buf.data, "abc&amp;&lt;&gt;&quot;&#39;def") == 0);
  string_buffer_free(&buf);
}

// Text with nothing to escape is copied through unchanged. This is the case the run batching exists
// for: the whole input is one run, appended once.
static void test_html_escape_copies_plain_text(void) {
  struct StringBuffer buf;
  string_buffer_init(&buf);
  const char* input = "plain text, no entities here";
  TEST_CHECK(html_escape_append_len(&buf, input, strlen(input)) == 0);
  TEST_CHECK(strcmp(buf.data, "plain text, no entities here") == 0);
  string_buffer_free(&buf);
}

// Every byte outside the five-entity set is copied verbatim, including bytes above 0x7F and control
// bytes XML forbids. The golden fixtures are not relied on to feed this function a non-ASCII byte,
// so this test is what checks the signedness of the `char` read by the escape switch. A plausible
// rewrite of that switch into a 256-entry table indexed by the byte would read out of bounds on the
// first accented character in an interpolated value.
static void test_html_escape_copies_high_and_control_bytes_verbatim(void) {
  struct StringBuffer buf;
  string_buffer_init(&buf);
  const char* input = "caf\xC3\xA9 &\x01";
  TEST_CHECK(html_escape_append_len(&buf, input, strlen(input)) == 0);
  TEST_CHECK(strcmp(buf.data, "caf\xC3\xA9 &amp;\x01") == 0);
  string_buffer_free(&buf);
}

// Special characters at the range boundaries and adjacent to each other escape with no run between
// them, which is where an off-by-one in the run bookkeeping would drop or duplicate bytes.
static void test_html_escape_handles_run_boundaries(void) {
  static const char* const cases[][2] = {
      {"&x", "&amp;x"},
      {"x&", "x&amp;"},
      {"&&", "&amp;&amp;"},
      {"&", "&amp;"},
      {"<a>&<b>", "&lt;a&gt;&amp;&lt;b&gt;"},
      {"a&&b", "a&amp;&amp;b"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct StringBuffer buf;
    string_buffer_init(&buf);
    TEST_CHECK(html_escape_append_len(&buf, cases[i][0], strlen(cases[i][0])) == 0);
    TEST_CHECK(buf.data != NULL && strcmp(buf.data, cases[i][1]) == 0);
    string_buffer_free(&buf);
  }
}

// Escaping appends to whatever the buffer already holds rather than replacing it.
static void test_html_escape_appends_to_existing_content(void) {
  struct StringBuffer buf;
  string_buffer_init(&buf);
  TEST_CHECK(string_buffer_append(&buf, "<pre>") == 0);
  const char* input = "a&b";
  TEST_CHECK(html_escape_append_len(&buf, input, strlen(input)) == 0);
  TEST_CHECK(strcmp(buf.data, "<pre>a&amp;b") == 0);
  string_buffer_free(&buf);
}

// Only the requested prefix is escaped, so a borrowed slice does not read past its length.
static void test_html_escape_honors_length(void) {
  struct StringBuffer buf;
  string_buffer_init(&buf);
  TEST_CHECK(html_escape_append_len(&buf, "a&b<c", 3) == 0);
  TEST_CHECK(strcmp(buf.data, "a&amp;b") == 0);
  string_buffer_free(&buf);
}

// Escaping an empty range is a no-op: it returns success and leaves the buffer untouched.
static void test_html_escape_empty_input(void) {
  struct StringBuffer buf;
  string_buffer_init(&buf);
  TEST_CHECK(html_escape_append_len(&buf, "", 0) == 0);
  TEST_CHECK(buf.len == 0);
  TEST_CHECK(buf.data == NULL);
  // The documented `NULL`-with-zero-length form, so `nonnull` covers `buffer` only. This is a
  // different pointer shape from the empty literal above, and it is the form a caller may rely on.
  TEST_CHECK(html_escape_append_len(&buf, NULL, 0) == 0);
  TEST_CHECK(buf.len == 0);
  TEST_CHECK(buf.data == NULL);
  string_buffer_free(&buf);
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
