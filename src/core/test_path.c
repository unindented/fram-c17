#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "core/path.h"
#include "shared/arena.h"
#include "shared/str.h"
#include "test_support.h"

// `path_check_output_limits` is the single place both output-path limits are applied. Two producers
// depend on it, so it is tested here rather than only through them. The function checks both limits
// in order and distinguishes them in the result: a path over `OUTPUT_PATH_RELATIVE_LEN_MAX` yields
// `PATH_OUTPUT_TOO_LONG` whatever its segments look like, while a path within that bound but
// holding one name over `FILENAME_LEN_MAX` yields `PATH_OUTPUT_SEGMENT_TOO_LONG`. The longest
// segment comes back whatever the verdict, so the segment diagnostic can name it rather than only
// the limit.
static void test_check_output_limits_reports_both_limits_and_segment(void) {
  struct Str segment;

  TEST_CHECK(path_check_output_limits(S("a/b.html"), &segment) == PATH_OUTPUT_OK);
  TEST_CHECK(str_eq(segment, S("b.html")));

  // The reported segment is the widest `/`-delimited name and the view points at it, wherever it
  // sits. An implementation that only measured the final name, or that dropped the running maximum
  // at a `/`, fails the middle and front cases.
  TEST_CHECK(path_check_output_limits(S("a/bbb/cc"), &segment) == PATH_OUTPUT_OK);
  TEST_CHECK(str_eq(segment, S("bbb")));
  TEST_CHECK(path_check_output_limits(S("bbb"), &segment) == PATH_OUTPUT_OK && segment.len == 3);
  TEST_CHECK(path_check_output_limits(S("a/bbb"), &segment) == PATH_OUTPUT_OK && segment.len == 3);
  TEST_CHECK(path_check_output_limits(S("bbb/a"), &segment) == PATH_OUTPUT_OK && segment.len == 3);

  // A trailing separator leaves an empty final name, which contributes nothing. A path with no name
  // content reports an empty segment that points at the path itself.
  TEST_CHECK(path_check_output_limits(S("aa/"), &segment) == PATH_OUTPUT_OK && segment.len == 2);
  const struct Str empty = S("");
  TEST_CHECK(path_check_output_limits(empty, &segment) == PATH_OUTPUT_OK);
  TEST_CHECK(segment.data == empty.data && segment.len == 0);
  const struct Str slashes = S("///");
  TEST_CHECK(path_check_output_limits(slashes, &segment) == PATH_OUTPUT_OK);
  TEST_CHECK(segment.data == slashes.data && segment.len == 0);

  // Exactly at each bound is still accepted, so both comparisons are `>` and not `>=`.
  char at_filename_limit[FILENAME_LEN_MAX];
  memset(at_filename_limit, 'a', FILENAME_LEN_MAX);
  TEST_CHECK(path_check_output_limits((struct Str){at_filename_limit, FILENAME_LEN_MAX},
                                      &segment) == PATH_OUTPUT_OK);
  TEST_CHECK(segment.len == FILENAME_LEN_MAX);

  // The whole-path bound, at exactly the limit, with every segment well inside the per-name limit
  // so only the whole-path comparison can reject it. Without this the `>` on the whole-path check
  // is unpinned while the `>` on the per-name check above is pinned.
  char at_path_limit[OUTPUT_PATH_RELATIVE_LEN_MAX];
  for (ptrdiff_t i = 0; i < OUTPUT_PATH_RELATIVE_LEN_MAX; i++) {
    at_path_limit[i] = (i + 1) % 4 == 0 ? '/' : 'a';
  }
  // A trailing separator would leave the final name empty, which is the separate case covered
  // above. Keep the path ending in name content so this is purely the at-bound whole-path check.
  if (at_path_limit[OUTPUT_PATH_RELATIVE_LEN_MAX - 1] == '/') {
    at_path_limit[OUTPUT_PATH_RELATIVE_LEN_MAX - 1] = 'a';
  }
  TEST_CHECK(path_check_output_limits((struct Str){at_path_limit, OUTPUT_PATH_RELATIVE_LEN_MAX},
                                      &segment) == PATH_OUTPUT_OK);
  TEST_CHECK(segment.len == 4);

  // One byte over the per-name limit, inside the whole-path limit: the segment verdict.
  char over_filename_limit[FILENAME_LEN_MAX + 1];
  memset(over_filename_limit, 'a', FILENAME_LEN_MAX + 1);
  TEST_CHECK(path_check_output_limits((struct Str){over_filename_limit, FILENAME_LEN_MAX + 1},
                                      &segment) == PATH_OUTPUT_SEGMENT_TOO_LONG);
  TEST_CHECK(segment.len == FILENAME_LEN_MAX + 1);

  // Over the whole-path limit while every segment stays inside the per-name limit, so this can only
  // be reported by the whole-path check.
  char over_path_limit[OUTPUT_PATH_RELATIVE_LEN_MAX + 8];
  ptrdiff_t written = 0;
  while (written + 4 < (ptrdiff_t)sizeof(over_path_limit)) {
    memcpy(over_path_limit + written, "aaa/", 4);
    written += 4;
  }
  TEST_CHECK(written > OUTPUT_PATH_RELATIVE_LEN_MAX);
  TEST_CHECK(path_check_output_limits((struct Str){over_path_limit, written}, &segment) ==
             PATH_OUTPUT_TOO_LONG);
  TEST_CHECK(segment.len == 3);
}

// `path_basename` is the part after the last `/`, the whole path when it has none, and empty when
// the path ends in `/`, each borrowed in place.
static void test_basename_takes_final_component(void) {
  const struct Str path = S("content/v1.2/a.md");
  const struct Str basename = path_basename(path);
  TEST_CHECK(str_eq(basename, S("a.md")));
  TEST_CHECK(basename.data == path.data + S("content/v1.2/").len);
  TEST_CHECK(str_eq(path_basename(S("a.md")), S("a.md")));
  TEST_CHECK(str_eq(path_basename(S("/a.md")), S("a.md")));
  TEST_CHECK(path_basename(S("content/")).len == 0);
  TEST_CHECK(path_basename(S("")).len == 0);
}

// `path_parent` is the part before the last `/`, without that `/`. A path with no `/` has an empty
// parent, and so does one whose only `/` is the leading one, so neither names a directory to
// create.
static void test_parent_takes_directory_part(void) {
  const struct Str path = S("content/v1.2/a.md");
  const struct Str parent = path_parent(path);
  TEST_CHECK(str_eq(parent, S("content/v1.2")));
  TEST_CHECK(parent.data == path.data);
  TEST_CHECK(str_eq(path_parent(S("content/")), S("content")));
  TEST_CHECK(str_eq(path_parent(S("/srv/a.md")), S("/srv")));
  TEST_CHECK(path_parent(S("a.md")).len == 0);
  TEST_CHECK(path_parent(S("/a.md")).len == 0);
  TEST_CHECK(path_parent(S("")).len == 0);
}

// `path_join` inserts exactly one `/` whether or not the directory already ends in one, and an
// empty base adds none.
static void test_join_inserts_separator(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(path_join(S("out"), S("a/index.html"), &arena), S("out/a/index.html")));
  TEST_CHECK(str_eq(path_join(S("out/"), S("a/index.html"), &arena), S("out/a/index.html")));
  TEST_CHECK(str_eq(path_join(S(""), S("a/index.html"), &arena), S("a/index.html")));
  TEST_CHECK(str_eq(path_join(S("out"), S(""), &arena), S("out/")));
}

// A path below the root yields the remainder after the root and its separator, borrowed in place.
static void test_is_below_returns_remainder_below_root(void) {
  const struct Str path = S("site/assets/icons/a.svg");
  struct Str relative = {0};
  TEST_CHECK(path_is_below(path, S("site/assets"), &relative));
  TEST_CHECK(str_eq(relative, S("icons/a.svg")));
  TEST_CHECK(relative.data == path.data + 12);
}

// A sibling directory sharing the root as a text prefix is not below it, because the byte after the
// root must be a separator.
static void test_is_below_rejects_sibling_sharing_prefix(void) {
  struct Str relative = {0};
  TEST_CHECK(!path_is_below(S("site/assets-old/a.svg"), S("site/assets"), &relative));
}

// A path outside the root, or the root itself, has no part below the root.
static void test_is_below_rejects_path_not_below_root(void) {
  struct Str relative = {0};
  TEST_CHECK(!path_is_below(S("other/a.svg"), S("site/assets"), &relative));
  TEST_CHECK(!path_is_below(S("site/assets"), S("site/assets"), &relative));
  TEST_CHECK(!path_is_below(S("site"), S("site/assets"), &relative));
  TEST_CHECK(relative.data == NULL);
}

// The safe-relative-path check accepts valid output and template names and rejects escapes.
static void test_safe_relative_path_accepts_valid_rejects_escapes(void) {
  TEST_CHECK(path_is_safe_relative(S("sections/about.html")));
  TEST_CHECK(path_is_safe_relative(S("atom.xml")));
  // These are the accept-side cases of the dot-segment boundary that the rejections below do not
  // cover. `.` and `..` are the only dot segments. A one-byte segment and a two-byte segment that
  // merely contains a `.` are ordinary names. Without these cases, `is_dot_segment` widened to
  // treat any one-byte segment or any segment touching a `.` as a traversal would still pass every
  // case here, while refusing to publish `albums/a/index.html`.
  TEST_CHECK(path_is_safe_relative(S("a/b.html")));
  TEST_CHECK(path_is_safe_relative(S("a")));
  TEST_CHECK(path_is_safe_relative(S(".a")));
  TEST_CHECK(path_is_safe_relative(S("a.")));
  TEST_CHECK(path_is_safe_relative(S("a/.b/c.")));
  TEST_CHECK(!path_is_safe_relative(S("../atom.xml")));
  TEST_CHECK(!path_is_safe_relative(S("sections/../atom.xml")));
  TEST_CHECK(!path_is_safe_relative(S("/atom.xml")));

  TEST_CHECK(!path_is_safe_relative(S("a\0b")));
  TEST_CHECK(!path_is_safe_relative(S("")));
  TEST_CHECK(!path_is_safe_relative(S("bad name.html")));
  TEST_CHECK(!path_is_safe_relative(S("sections//about.html")));
  TEST_CHECK(!path_is_safe_relative(S("./about.html")));

  // The check rejects every byte above 0x7F. That is what the header's normalization argument rests
  // on: no accepted name has a second spelling, so two names that differ only as NFC and NFD cannot
  // both pass and then collide as one file on a normalizing filesystem.
  TEST_CHECK(!path_is_safe_relative(S("caf\xc3\xa9.html")));
  TEST_CHECK(!path_is_safe_relative(S("\xff")));
}

TEST_LIST = {
    {"check output limits reports both limits and segment",
     test_check_output_limits_reports_both_limits_and_segment},
    {"basename takes final component", test_basename_takes_final_component},
    {"parent takes directory part", test_parent_takes_directory_part},
    {"join inserts separator", test_join_inserts_separator},
    {"is below returns remainder below root", test_is_below_returns_remainder_below_root},
    {"is below rejects sibling sharing prefix", test_is_below_rejects_sibling_sharing_prefix},
    {"is below rejects path not below root", test_is_below_rejects_path_not_below_root},
    {"safe relative path accepts valid rejects escapes",
     test_safe_relative_path_accepts_valid_rejects_escapes},
    {NULL, NULL},
};
