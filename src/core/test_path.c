#include <acutest.h>
#include <string.h>

#include "core/path.h"
#include "shared/arena.h"

// `path_check_output_limits` is the single place both output-path limits are applied. Two producers
// depend on it, so it is tested here rather than only through them. The function checks both limits
// in order and distinguishes them in the result: a path over `OUTPUT_PATH_RELATIVE_LEN_MAX` yields
// `PATH_OUTPUT_TOO_LONG` whatever its segments look like, while a path within that bound but
// holding one name over `FILENAME_LEN_MAX` yields `PATH_OUTPUT_SEGMENT_TOO_LONG`. The measurements
// come back whatever the verdict, which lets both diagnostics name the offending value rather than
// only the limit.
static void test_check_output_limits_reports_both_limits_and_metrics(void) {
  struct PathOutputMetrics metrics;

  TEST_CHECK(path_check_output_limits("a/b.html", &metrics) == PATH_OUTPUT_OK);
  TEST_CHECK(metrics.len == strlen("a/b.html"));
  TEST_CHECK(metrics.segment_len == strlen("b.html"));
  TEST_CHECK(strncmp(metrics.segment, "b.html", metrics.segment_len) == 0);

  // The reported segment is the widest `/`-delimited name and the metrics point at it, wherever it
  // sits. An implementation that only measured the final name, or that dropped the running maximum
  // at a `/`, fails the middle and front cases.
  TEST_CHECK(path_check_output_limits("a/bbb/cc", &metrics) == PATH_OUTPUT_OK);
  TEST_CHECK(metrics.segment_len == 3 && strncmp(metrics.segment, "bbb", 3) == 0);
  TEST_CHECK(path_check_output_limits("bbb", &metrics) == PATH_OUTPUT_OK &&
             metrics.segment_len == 3);
  TEST_CHECK(path_check_output_limits("a/bbb", &metrics) == PATH_OUTPUT_OK &&
             metrics.segment_len == 3);
  TEST_CHECK(path_check_output_limits("bbb/a", &metrics) == PATH_OUTPUT_OK &&
             metrics.segment_len == 3);

  // A trailing separator leaves an empty final name, which contributes nothing. A path with no name
  // content reports `0` and points at the path itself, so a caller's `%.*s` prints nothing rather
  // than reading past the end.
  TEST_CHECK(path_check_output_limits("aa/", &metrics) == PATH_OUTPUT_OK &&
             metrics.segment_len == 2);
  const char* empty = "";
  TEST_CHECK(path_check_output_limits(empty, &metrics) == PATH_OUTPUT_OK);
  TEST_CHECK(metrics.segment == empty && metrics.segment_len == 0 && metrics.len == 0);
  const char* slashes = "///";
  TEST_CHECK(path_check_output_limits(slashes, &metrics) == PATH_OUTPUT_OK);
  TEST_CHECK(metrics.segment == slashes && metrics.segment_len == 0);

  // Exactly at each bound is still accepted, so both comparisons are `>` and not `>=`.
  char at_filename_limit[FILENAME_LEN_MAX + 1];
  memset(at_filename_limit, 'a', FILENAME_LEN_MAX);
  at_filename_limit[FILENAME_LEN_MAX] = '\0';
  TEST_CHECK(path_check_output_limits(at_filename_limit, &metrics) == PATH_OUTPUT_OK);
  TEST_CHECK(metrics.segment_len == FILENAME_LEN_MAX);

  // The whole-path bound, at exactly the limit, with every segment well inside the per-name limit
  // so only the whole-path comparison can reject it. Without this the `>` on the whole-path check
  // is unpinned while the `>` on the per-name check above is pinned.
  char at_path_limit[OUTPUT_PATH_RELATIVE_LEN_MAX + 1];
  for (size_t i = 0; i < (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX; i++) {
    at_path_limit[i] = (i + 1) % 4 == 0 ? '/' : 'a';
  }
  at_path_limit[OUTPUT_PATH_RELATIVE_LEN_MAX] = '\0';
  // A trailing separator would leave the final name empty, which is the separate case covered
  // above. Keep the path ending in name content so this is purely the at-bound whole-path check.
  if (at_path_limit[OUTPUT_PATH_RELATIVE_LEN_MAX - 1] == '/') {
    at_path_limit[OUTPUT_PATH_RELATIVE_LEN_MAX - 1] = 'a';
  }
  TEST_CHECK(path_check_output_limits(at_path_limit, &metrics) == PATH_OUTPUT_OK);
  TEST_CHECK(metrics.len == (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX);
  TEST_CHECK(metrics.segment_len == 4);

  // One byte over the per-name limit, inside the whole-path limit: the segment verdict.
  char over_filename_limit[FILENAME_LEN_MAX + 2];
  memset(over_filename_limit, 'a', FILENAME_LEN_MAX + 1);
  over_filename_limit[FILENAME_LEN_MAX + 1] = '\0';
  TEST_CHECK(path_check_output_limits(over_filename_limit, &metrics) ==
             PATH_OUTPUT_SEGMENT_TOO_LONG);
  TEST_CHECK(metrics.segment_len == (size_t)FILENAME_LEN_MAX + 1);
  TEST_CHECK(metrics.len == (size_t)FILENAME_LEN_MAX + 1);

  // Over the whole-path limit while every segment stays inside the per-name limit, so this can only
  // be reported by the whole-path check.
  char over_path_limit[OUTPUT_PATH_RELATIVE_LEN_MAX + 8];
  size_t written = 0;
  while (written + 4 < sizeof(over_path_limit) - 1) {
    memcpy(over_path_limit + written, "aaa/", 4);
    written += 4;
  }
  over_path_limit[written] = '\0';
  TEST_CHECK(written > OUTPUT_PATH_RELATIVE_LEN_MAX);
  TEST_CHECK(path_check_output_limits(over_path_limit, &metrics) == PATH_OUTPUT_TOO_LONG);
  TEST_CHECK(metrics.len == written);
  TEST_CHECK(metrics.segment_len == 3);
}

// `path_join` inserts exactly one `/` whether or not the directory already ends in one.
static void test_join_inserts_separator(void) {
  struct Arena arena;
  arena_init(&arena);
  TEST_CHECK(strcmp(path_join("out", "a/index.html", &arena), "out/a/index.html") == 0);
  TEST_CHECK(strcmp(path_join("out/", "a/index.html", &arena), "out/a/index.html") == 0);
  arena_free(&arena);
}

// A path below the root yields the remainder after the root and its separator, borrowed in place.
static void test_relative_below_returns_remainder_below_root(void) {
  const char* path = "site/assets/icons/a.svg";
  const char* relative = path_relative_below(path, "site/assets");
  TEST_ASSERT(relative != NULL);
  TEST_CHECK(strcmp(relative, "icons/a.svg") == 0);
  TEST_CHECK(relative == path + strlen("site/assets/"));
}

// A sibling directory sharing the root as a text prefix is not below it, because the byte after the
// root must be a separator.
static void test_relative_below_rejects_sibling_sharing_prefix(void) {
  TEST_CHECK(path_relative_below("site/assets-old/a.svg", "site/assets") == NULL);
}

// A path outside the root, or the root itself, has no part below the root.
static void test_relative_below_rejects_path_not_below_root(void) {
  TEST_CHECK(path_relative_below("other/a.svg", "site/assets") == NULL);
  TEST_CHECK(path_relative_below("site/assets", "site/assets") == NULL);
  TEST_CHECK(path_relative_below("site", "site/assets") == NULL);
}

// The safe-relative-path check accepts valid output and template names and rejects escapes.
static void test_safe_relative_path_accepts_valid_rejects_escapes(void) {
  TEST_CHECK(path_is_safe_relative("sections/about.html"));
  TEST_CHECK(path_is_safe_relative("atom.xml"));
  // These are the accept-side cases of the dot-segment boundary that the rejections below do not
  // cover. `.` and `..` are the only dot segments. A one-byte segment and a two-byte segment that
  // merely contains a `.` are ordinary names. Without these cases, `is_dot_segment` widened to
  // treat any one-byte segment or any segment touching a `.` as a traversal would still pass every
  // case here, while refusing to publish `albums/a/index.html`.
  TEST_CHECK(path_is_safe_relative("a/b.html"));
  TEST_CHECK(path_is_safe_relative("a"));
  TEST_CHECK(path_is_safe_relative(".a"));
  TEST_CHECK(path_is_safe_relative("a."));
  TEST_CHECK(path_is_safe_relative("a/.b/c."));
  TEST_CHECK(!path_is_safe_relative("../atom.xml"));
  TEST_CHECK(!path_is_safe_relative("sections/../atom.xml"));
  TEST_CHECK(!path_is_safe_relative("/atom.xml"));

  TEST_CHECK(!path_is_safe_relative(NULL));
  TEST_CHECK(!path_is_safe_relative(""));
  TEST_CHECK(!path_is_safe_relative("bad name.html"));
  TEST_CHECK(!path_is_safe_relative("sections//about.html"));
  TEST_CHECK(!path_is_safe_relative("./about.html"));

  // The check rejects every byte above 0x7F. That is what the header's normalization argument rests
  // on: no accepted name has a second spelling, so two names that differ only as NFC and NFD cannot
  // both pass and then collide as one file on a normalizing filesystem.
  TEST_CHECK(!path_is_safe_relative("caf\xc3\xa9.html"));
  TEST_CHECK(!path_is_safe_relative("\xff"));
}

TEST_LIST = {
    {"check output limits reports both limits and metrics",
     test_check_output_limits_reports_both_limits_and_metrics},
    {"join inserts separator", test_join_inserts_separator},
    {"relative below returns remainder below root",
     test_relative_below_returns_remainder_below_root},
    {"relative below rejects sibling sharing prefix",
     test_relative_below_rejects_sibling_sharing_prefix},
    {"relative below rejects path not below root", test_relative_below_rejects_path_not_below_root},
    {"safe relative path accepts valid rejects escapes",
     test_safe_relative_path_accepts_valid_rejects_escapes},
    {NULL, NULL},
};
