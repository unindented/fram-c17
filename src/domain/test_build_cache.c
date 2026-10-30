#include <acutest.h>
#include <stddef.h>

#include "domain/build_cache.h"
#include "formats/image.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * @brief Formats a build cache of `entries` the way a build writes one.
 *
 * @param entries     Rows to format, sorted by path. May be `NULL` only when `entry_count` is 0.
 * @param entry_count Number of rows in `entries`.
 * @param arena       Arena that owns the text. Must not be `NULL`.
 * @return The text, owned by `arena`.
 */
static struct Str format_cache(const struct BuildCacheEntry* entries,
                               ptrdiff_t entry_count,
                               struct Arena* arena) {
  struct Buf text = {.arena = arena};
  build_cache_format_header(&text);
  for (ptrdiff_t i = 0; i < entry_count; i++) {
    build_cache_format_entry(&text, &entries[i]);
  }
  return buf_str(&text);
}

/**
 * @brief Returns the stat a source must have to match `entry`.
 *
 * @param entry Row the stat matches. Must not be `NULL`.
 * @return The stat.
 */
static struct OsStat stat_of(const struct BuildCacheEntry* entry) {
  return (struct OsStat){.len = entry->len, .mtime_s = entry->mtime_s, .mtime_ns = entry->mtime_ns};
}

/** Two rows, an image and a video, in path order. */
static const struct BuildCacheEntry ENTRIES[] = {
    {.path = STR_INIT("album/a.jpg"),
     .len = 1234,
     .mtime_s = 1782864000,
     .mtime_ns = 5,
     .width_px = 24,
     .height_px = 12,
     .orientation = IMAGE_ORIENTATION_RIGHT_TOP},
    {.path = STR_INIT("clip.mp4"),
     .len = 99,
     .mtime_s = 1782864001,
     .mtime_ns = 999999999,
     .width_px = 640,
     .height_px = 360,
     .orientation = IMAGE_ORIENTATION_TOP_LEFT,
     .duration_ms = 1500},
};

// A build cache a build wrote parses back to the same rows, and a row is found only while the
// source's size and modification time still match it.
static void test_round_trips_and_finds_matching_rows(void) {
  struct Arena arena = test_arena();
  const struct Str text = format_cache(ENTRIES, 2, &arena);
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_CHECK(build_cache_parse(text, &arena, &cache, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(cache.count == 2);

  struct OsStat stat = stat_of(&ENTRIES[1]);
  const struct BuildCacheEntry* found = build_cache_find(&cache, S("clip.mp4"), &stat);
  TEST_CHECK(found != NULL && found->width_px == 640 && found->height_px == 360 &&
             found->orientation == IMAGE_ORIENTATION_TOP_LEFT && found->duration_ms == 1500);
  stat = stat_of(&ENTRIES[0]);
  found = build_cache_find(&cache, S("album/a.jpg"), &stat);
  TEST_CHECK(found != NULL && found->orientation == IMAGE_ORIENTATION_RIGHT_TOP);

  stat.len++;
  TEST_CHECK(build_cache_find(&cache, S("album/a.jpg"), &stat) == NULL);
  stat = stat_of(&ENTRIES[0]);
  stat.mtime_ns++;
  TEST_CHECK(build_cache_find(&cache, S("album/a.jpg"), &stat) == NULL);
  TEST_CHECK(build_cache_find(&cache, S("album/b.jpg"), &stat) == NULL);
}

// A gallery with no media writes the first line alone, which parses as an empty build cache.
static void test_parses_first_line_alone(void) {
  struct Arena arena = test_arena();
  const struct Str text = format_cache(NULL, 0, &arena);
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_CHECK(build_cache_parse(text, &arena, &cache, &log) == 0);
  TEST_CHECK(cache.count == 0);
}

// A source whose relative path holds a tab or a newline gets no row, so it is probed every build
// and no path needs escaping.
static void test_skips_path_with_tab_or_newline(void) {
  struct Arena arena = test_arena();
  struct Buf text = {.arena = &arena};
  struct BuildCacheEntry entry = ENTRIES[0];
  entry.path = S("album/a\tb.jpg");
  build_cache_format_entry(&text, &entry);
  entry.path = S("album/a\nb.jpg");
  build_cache_format_entry(&text, &entry);
  TEST_CHECK(buf_str(&text).len == 0);
}

// A first line written by another generator is an empty build cache, reported for the verbose log.
static void test_rejects_another_build(void) {
  struct Arena arena = test_arena();
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_CHECK(build_cache_parse(S("fram-build-cache\tfram 0.0.0-other\n"), &arena, &cache, &log) ==
             -1);
  TEST_CHECK(test_log_is(&log, "build cache was written by another build\n"));
  TEST_CHECK(cache.count == 0);
}

// A file that does not parse is an empty build cache, reported with the first line that does not: a
// row cut short by an interrupted write, a field out of range, and rows out of order.
static void test_rejects_rows_that_do_not_parse(void) {
  struct Arena arena = test_arena();
  const struct Str text = format_cache(ENTRIES, 2, &arena);
  static const char* const bad_rows[] = {
      "album/a.jpg\t1\t2\t3\t4\t5\t1\t0",             // no newline
      "album/a.jpg\t1\t2\t3\t4\t5\t9\t0\n",           // orientation out of range
      "album/a.jpg\t1\t2\t1000000000\t4\t5\t1\t0\n",  // nanoseconds out of range
      "album/a.jpg\t1\t2\t3\t4\t5\t1\n",              // a field missing
      "album/a.jpg\t1\t2\t3\t4\t5\t1\t0\t0\n",        // a field extra
      "album/a.jpg\t-1\t2\t3\t4\t5\t1\t0\n",          // a negative size
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(bad_rows) / sizeof(bad_rows[0])); i++) {
    struct Buf bad = {.arena = &arena};
    buf_append(&bad, str_head(text, str_cut(text, '\n').head.len + 1));
    buf_append(&bad, str_from_cstr(bad_rows[i]));
    struct BuildCache cache = {0};
    struct Log log = test_log();
    TEST_CHECK_(build_cache_parse(buf_str(&bad), &arena, &cache, &log) == -1, "row %td", i);
    TEST_CHECK(test_log_is(&log, "build cache does not parse at line 2\n"));
  }

  struct BuildCacheEntry reversed[] = {ENTRIES[1], ENTRIES[0]};
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_CHECK(build_cache_parse(format_cache(reversed, 2, &arena), &arena, &cache, &log) == -1);
  TEST_CHECK(test_log_is(&log, "build cache does not parse at line 3\n"));
}

TEST_LIST = {
    {"round trips and finds matching rows", test_round_trips_and_finds_matching_rows},
    {"parses first line alone", test_parses_first_line_alone},
    {"skips path with tab or newline", test_skips_path_with_tab_or_newline},
    {"rejects another build", test_rejects_another_build},
    {"rejects rows that do not parse", test_rejects_rows_that_do_not_parse},
    {NULL, NULL},
};
