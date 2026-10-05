#include <acutest.h>
#include <stdio.h>
#include <string.h>

#include "build/output_path.h"
#include "core/error.h"
#include "core/path.h"
#include "shared/arena.h"

// A path within both limits is accepted and leaves the diagnostic buffer untouched.
static void test_check_limits_accepts_bounded_path(void) {
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(output_path_check_limits("album/index.html", "album", err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
}

// Each `/` in an output file path adds one `../`, so a top-level file gets an empty prefix.
static void test_to_root_climbs_one_level_per_separator(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* top = output_path_to_root("index.html", &arena);
  const char* nested = output_path_to_root("a/b/index.html", &arena);
  TEST_ASSERT(top != NULL && nested != NULL);
  TEST_CHECK(strcmp(top, "") == 0);
  TEST_CHECK(strcmp(nested, "../../") == 0);
  arena_free(&arena);
}

// A path over the whole-path limit names the limit, the measured length, and the producer ahead of
// the path, so the numbers survive the path being cut off.
static void test_check_limits_rejects_overlong_path(void) {
  enum { SEGMENT_LEN = 200, SEGMENT_COUNT = 6 };
  char path[SEGMENT_COUNT * (SEGMENT_LEN + 1)];
  size_t path_len = 0;
  for (size_t i = 0; i < (size_t)SEGMENT_COUNT; i++) {
    if (i > 0) {
      path[path_len++] = '/';
    }
    memset(path + path_len, 'a', SEGMENT_LEN);
    path_len += SEGMENT_LEN;
  }
  path[path_len] = '\0';
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(output_path_check_limits(path, "src.jpg", err, sizeof(err)) == -1);
  char expected_head[128];
  const int expected_head_len = snprintf(
      expected_head, sizeof(expected_head),
      "output path exceeds max output path length (%zu bytes) at %zu bytes (for 'src.jpg'): 'aaa",
      (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX, path_len);
  TEST_CHECK(expected_head_len > 0 && (size_t)expected_head_len < sizeof(expected_head));
  TEST_CHECK(strncmp(err, expected_head, (size_t)expected_head_len) == 0);
  TEST_MSG("err: %s", err);
}

// A path within the whole-path limit but with one overlong segment names only that segment as the
// offending value.
static void test_check_limits_rejects_overlong_segment(void) {
  char segment[FILENAME_LEN_MAX + 2];
  memset(segment, 'b', sizeof(segment) - 1);
  segment[sizeof(segment) - 1] = '\0';
  char path[sizeof("album/") + sizeof(segment)];
  (void)snprintf(path, sizeof(path), "album/%s", segment);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(output_path_check_limits(path, "src.jpg", err, sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected),
                                    "output path segment exceeds max filename length (%zu bytes) "
                                    "at %zu bytes (for 'src.jpg'): '%s'",
                                    (size_t)FILENAME_LEN_MAX, sizeof(segment) - 1, segment);
  TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("err: %s", err);
}

TEST_LIST = {
    {"check limits accepts bounded path", test_check_limits_accepts_bounded_path},
    {"to root climbs one level per separator", test_to_root_climbs_one_level_per_separator},
    {"check limits rejects overlong path", test_check_limits_rejects_overlong_path},
    {"check limits rejects overlong segment", test_check_limits_rejects_overlong_segment},
    {NULL, NULL},
};
