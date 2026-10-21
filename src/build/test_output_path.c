#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "build/output_path.h"
#include "core/path.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// A path within both limits is accepted and records nothing.
static void test_check_limits_accepts_bounded_path(void) {
  struct Log log = test_log();
  TEST_CHECK(output_path_check_limits(S("album/index.html"), S("album"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// Each `/` in an output file path adds one `../`, so a top-level file gets an empty prefix.
static void test_to_root_climbs_one_level_per_separator(void) {
  struct Arena arena = test_arena();
  struct Str top = output_path_to_root(S("index.html"), &arena);
  struct Str nested = output_path_to_root(S("a/b/index.html"), &arena);
  TEST_CHECK(top.len == 0);
  TEST_CHECK(str_eq(nested, S("../../")));
}

// A path over the whole-path limit names the limit, the measured length, the producer, and the
// whole path.
static void test_check_limits_rejects_overlong_path(void) {
  enum { SEGMENT_LEN = 200, SEGMENT_COUNT = 6 };
  char path[SEGMENT_COUNT * (SEGMENT_LEN + 1)];
  ptrdiff_t path_len = 0;
  for (ptrdiff_t i = 0; i < SEGMENT_COUNT; i++) {
    if (i > 0) {
      path[path_len++] = '/';
    }
    memset(path + path_len, 'a', SEGMENT_LEN);
    path_len += SEGMENT_LEN;
  }
  const struct Str overlong = {path, path_len};
  struct Log log = test_log();
  TEST_CHECK(output_path_check_limits(overlong, S("src.jpg"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'src.jpg' exceeds max output path length (%d bytes) at "
                         "%td bytes: '%.*s'\n",
                         OUTPUT_PATH_RELATIVE_LEN_MAX, path_len, STR_FMT(overlong)));
  TEST_CHECK(log.errors == 1);
}

// A path within the whole-path limit but with one overlong segment names only that segment as the
// offending value.
static void test_check_limits_rejects_overlong_segment(void) {
  enum { SEGMENT_LEN = FILENAME_LEN_MAX + 1 };
  char path[sizeof("album/") - 1 + SEGMENT_LEN];
  memcpy(path, "album/", sizeof("album/") - 1);
  memset(path + sizeof("album/") - 1, 'b', SEGMENT_LEN);
  const struct Str segment = {path + sizeof("album/") - 1, SEGMENT_LEN};
  const struct Str whole = {path, (ptrdiff_t)sizeof(path)};
  struct Log log = test_log();
  TEST_CHECK(output_path_check_limits(whole, S("src.jpg"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path segment for 'src.jpg' exceeds max filename length (%d "
                         "bytes) at %d bytes: '%.*s'\n",
                         FILENAME_LEN_MAX, SEGMENT_LEN, STR_FMT(segment)));
}

TEST_LIST = {
    {"check limits accepts bounded path", test_check_limits_accepts_bounded_path},
    {"to root climbs one level per separator", test_to_root_climbs_one_level_per_separator},
    {"check limits rejects overlong path", test_check_limits_rejects_overlong_path},
    {"check limits rejects overlong segment", test_check_limits_rejects_overlong_segment},
    {NULL, NULL},
};
