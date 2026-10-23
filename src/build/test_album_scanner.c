#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build/album_scanner.h"
#include "build/job.h"
#include "build/test_album_scanner.h"
#include "build/test_job_arenas.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/media_item.h"
#include "formats/image.h"
#include "os_memory.h"
#include "runtime/video.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_jpeg_fixture.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few hundred bytes. */
enum { TEST_FILE_LEN_MAX = 1024 * 1024 };

/**
 * @brief Inserts an EXIF segment carrying the right-top orientation into a JPEG.
 *
 * The segment goes directly after the SOI marker, ahead of the encoder's own segments.
 *
 * @param jpeg     JPEG to copy, at least 2 bytes long.
 * @param arena    Arena that owns the oriented JPEG. Must not be `NULL`.
 * @param jpeg_out Receives the oriented JPEG, which lives as long as `arena`. Must not be `NULL`.
 * @return `0` on success, or `-1` on a short input.
 */
static int add_right_top_exif(struct Str jpeg, struct Arena* arena, struct Str* jpeg_out) {
  static const unsigned char segment[] = {
      0xFF,
      0xE1,
      0x00,
      0x22,
      'E',
      'x',
      'i',
      'f',
      0x00,
      0x00,
      'I',
      'I',
      0x2A,
      0x00,
      0x08,
      0x00,
      0x00,
      0x00,
      0x01,
      0x00,
      0x12,
      0x01,
      0x03,
      0x00,
      0x01,
      0x00,
      0x00,
      0x00,
      IMAGE_ORIENTATION_RIGHT_TOP,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
  };
  if (jpeg.len < 2) {
    return -1;
  }
  struct Buf oriented = {.arena = arena};
  buf_append(&oriented, str_head(jpeg, 2));
  buf_append(&oriented, str_from_bytes(segment, (ptrdiff_t)sizeof(segment)));
  buf_append(&oriented, str_tail(jpeg, 2));
  *jpeg_out = buf_str(&oriented);
  return 0;
}

/**
 * @brief Builds the media source path `/photos/<stem>.jpg`, whose stem repeats one byte.
 *
 * @param fill     Byte the stem repeats.
 * @param stem_len Length of the stem in bytes.
 * @param arena    Arena that owns the path. Must not be `NULL`.
 * @return The source path, owned by `arena`.
 */
static struct Str long_source_path(char fill, ptrdiff_t stem_len, struct Arena* arena) {
  struct Buf path = {.arena = arena};
  buf_append(&path, S("/photos/"));
  for (ptrdiff_t i = 0; i < stem_len; i++) {
    buf_append_char(&path, fill);
  }
  buf_append(&path, S(".jpg"));
  return buf_str(&path);
}

/**
 * @brief Builds the skeleton of the sort fixture under the given album and media orders.
 *
 * The fixture's root holds the images `a`, `b` and `c` and the albums `x`, `y` and `z`. `x` holds
 * two images and the albums `x/p` and `x/q`, which hold one each. `y` holds one image. `z` holds no
 * image of its own and the albums `z/m` and `z/n`, which hold one each. So some array of each kind
 * has zero, one, and several entries. In source order the albums are `""`, `x`, `x/p`, `x/q`, `y`,
 * `z`, `z/m`, `z/n`, and the media are `a`, `b`, `c`, `x/1`, `x/2`, `x/p/9`, `x/q/8`, `y/only`,
 * `z/m/one`, `z/n/two`.
 *
 * @param album_sort_order Order of each album's `sub_albums`.
 * @param media_sort_order Order of each album's `media`.
 * @param perm             Arena that owns the skeleton. Must not be `NULL`.
 * @param albums_out       Receives the album array, sorted by `source_dir`. Must not be `NULL`.
 * @param media_out        Receives the flat media array. Must not be `NULL`.
 * @return `0` when the skeleton was built with the fixture's 8 albums and 10 media items, or `-1`
 *         after recording a test failure.
 */
static int build_sort_fixture(enum GallerySortOrder album_sort_order,
                              enum GallerySortOrder media_sort_order,
                              struct Arena* perm,
                              struct Album*** albums_out,
                              struct MediaItem*** media_out) {
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  config.album_sort_order = album_sort_order;
  config.media_sort_order = media_sort_order;
  struct Str items[] = {
      S("/photos/a.jpg"),       S("/photos/b.jpg"),      S("/photos/c.jpg"),
      S("/photos/x/1.jpg"),     S("/photos/x/2.jpg"),    S("/photos/x/p/9.jpg"),
      S("/photos/x/q/8.jpg"),   S("/photos/y/only.jpg"), S("/photos/z/m/one.jpg"),
      S("/photos/z/n/two.jpg"),
  };
  const ptrdiff_t item_count = (ptrdiff_t)(sizeof(items) / sizeof(items[0]));
  const struct PathList paths = {.items = items, .count = item_count, .capacity = item_count};
  ptrdiff_t album_count = 0;
  ptrdiff_t media_count = 0;
  struct Log log = test_log();
  if (!TEST_CHECK(album_scanner_build_skeleton(&config, &paths, perm, test_arena(), albums_out,
                                               &album_count, media_out, &media_count, &log) == 0)) {
    return -1;
  }
  TEST_CHECK(test_log_is_empty(&log));
  return TEST_CHECK(album_count == 8 && media_count == 10) ? 0 : -1;
}

// An empty source tree still produces the root page and no media.
static void test_build_skeleton_synthesizes_empty_root(void) {
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct Arena perm = test_arena();
  const struct PathList paths = {0};
  struct Album** albums = NULL;
  ptrdiff_t album_count = 0;
  struct MediaItem** media = NULL;
  ptrdiff_t media_count = 0;
  struct Log log = test_log();
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &perm, test_arena(), &albums,
                                           &album_count, &media, &media_count, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(album_count == 1);
  TEST_CHECK(media_count == 0);
  TEST_CHECK(str_eq(albums[0]->source_dir, S("")));
  TEST_CHECK(str_eq(albums[0]->title, S("Gallery")));
  TEST_CHECK(str_eq(albums[0]->url_path, S("index.html")));
  TEST_CHECK(str_eq(albums[0]->output_path, S("/public/index.html")));
  TEST_CHECK(str_eq(albums[0]->path_to_root, S("")));
  TEST_CHECK(albums[0]->cover == NULL);
}

// Sorted paths form a deterministic recursive tree with recipe paths, URLs, covers, and totals.
static void test_build_skeleton_derives_nested_gallery(void) {
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct Arena perm = test_arena();
  struct Str items[] = {
      S("/photos/2024/Japan/a #.JPG"),
      S("/photos/2024/Japan/clip.MP4"),
      S("/photos/2024/root.jpeg"),
      S("/photos/z.jpg"),
  };
  const struct PathList paths = {.items = items, .count = 4, .capacity = 4};
  struct Album** albums = NULL;
  ptrdiff_t album_count = 0;
  struct MediaItem** media = NULL;
  ptrdiff_t media_count = 0;
  struct Log log = test_log();
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &perm, test_arena(), &albums,
                                           &album_count, &media, &media_count, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(album_count == 3);
  TEST_CHECK(media_count == 4);
  TEST_CHECK(str_eq(albums[0]->source_dir, S("")));
  TEST_CHECK(str_eq(albums[1]->source_dir, S("2024")));
  TEST_CHECK(str_eq(albums[2]->source_dir, S("2024/Japan")));
  TEST_CHECK(str_eq(albums[2]->slug_path, S("2024/japan")));
  TEST_CHECK(albums[1]->parent == albums[0]);
  TEST_CHECK(albums[2]->parent == albums[1]);
  TEST_CHECK(albums[0]->sub_albums[0] == albums[1]);
  TEST_CHECK(albums[1]->sub_albums[0] == albums[2]);
  TEST_CHECK(str_eq(albums[2]->path_to_root, S("../../")));
  TEST_CHECK(str_eq(albums[2]->url_path, S("2024/japan/index.html")));

  TEST_CHECK(str_eq(media[0]->title, S("a #.JPG")));
  TEST_CHECK(media[0]->kind == MEDIA_KIND_IMAGE);
  TEST_CHECK(media[1]->kind == MEDIA_KIND_VIDEO);
  TEST_CHECK(media[0]->derivative_count == 3);
  TEST_CHECK(str_eq(media[0]->derivatives[0].name, S("s")));
  TEST_CHECK(str_eq(media[0]->derivatives[0].url,
                    S("_fram/v2-s-120x120c-q70-m-520x360c-q80-l-792x594-q85-f1000/"
                      "2024/japan/a-jpg-s.webp")));
  TEST_CHECK(str_eq(media[0]->derivatives[0].path,
                    S("/public/_fram/v2-s-120x120c-q70-m-520x360c-q80-l-792x594-q85-f1000/"
                      "2024/japan/a-jpg-s.webp")));
  TEST_CHECK(str_eq(media[1]->original_url, S("_fram/originals/2024/japan/clip-mp4.mp4")));
  TEST_CHECK(albums[2]->media_count == 2);
  TEST_CHECK(albums[2]->media[0] == media[0]);
  TEST_CHECK(albums[2]->media[1] == media[1]);

  TEST_CHECK(albums[2]->item_count_total == 2);
  TEST_CHECK(albums[1]->item_count_total == 3);
  TEST_CHECK(albums[0]->item_count_total == 4);
  TEST_CHECK(albums[2]->cover == media[0]);
  TEST_CHECK(albums[1]->cover == media[2]);
  TEST_CHECK(albums[0]->cover == media[3]);
}

// Source-file ordering can discover `a-c` before ancestor `a`. The album index remains sorted so
// binary lookup and reverse child aggregation stay valid.
static void test_build_skeleton_sorts_late_ancestors(void) {
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct Arena perm = test_arena();
  struct Str items[] = {
      S("/photos/a-c/file.jpg"),
      S("/photos/a/b/file.jpg"),
  };
  const struct PathList paths = {.items = items, .count = 2, .capacity = 2};
  struct Album** albums = NULL;
  ptrdiff_t album_count = 0;
  struct MediaItem** media = NULL;
  ptrdiff_t media_count = 0;
  struct Log log = test_log();
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &perm, test_arena(), &albums,
                                           &album_count, &media, &media_count, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(album_count == 4);
  TEST_CHECK(str_eq(albums[0]->source_dir, S("")));
  TEST_CHECK(str_eq(albums[1]->source_dir, S("a")));
  TEST_CHECK(str_eq(albums[2]->source_dir, S("a-c")));
  TEST_CHECK(str_eq(albums[3]->source_dir, S("a/b")));
  TEST_CHECK(albums[3]->parent == albums[1]);
  TEST_CHECK(albums[0]->sub_album_count == 2);
  TEST_CHECK(albums[0]->sub_albums[0] == albums[1]);
  TEST_CHECK(albums[0]->sub_albums[1] == albums[2]);
  TEST_CHECK(albums[1]->sub_albums[0] == albums[3]);
  TEST_CHECK(albums[0]->item_count_total == 2);
}

// An album discovered after many siblings that sort after it is inserted at its sorted place, far
// from the end of the index, and every later lookup still finds it and its siblings.
static void test_build_skeleton_inserts_late_album_before_siblings(void) {
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct Arena perm = test_arena();
  struct Str items[] = {
      S("/photos/a-1/file.jpg"), S("/photos/a-2/file.jpg"), S("/photos/a-3/file.jpg"),
      S("/photos/a-4/file.jpg"), S("/photos/a-5/file.jpg"), S("/photos/a-6/file.jpg"),
      S("/photos/a-7/file.jpg"), S("/photos/a/b/file.jpg"), S("/photos/a/file.jpg"),
  };
  const struct PathList paths = {.items = items, .count = 9, .capacity = 9};
  struct Album** albums = NULL;
  ptrdiff_t album_count = 0;
  struct MediaItem** media = NULL;
  ptrdiff_t media_count = 0;
  struct Log log = test_log();
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &perm, test_arena(), &albums,
                                           &album_count, &media, &media_count, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  static const struct Str source_dirs[] = {
      STR_INIT(""),    STR_INIT("a"),   STR_INIT("a-1"), STR_INIT("a-2"), STR_INIT("a-3"),
      STR_INIT("a-4"), STR_INIT("a-5"), STR_INIT("a-6"), STR_INIT("a-7"), STR_INIT("a/b")};
  TEST_ASSERT(album_count == (ptrdiff_t)(sizeof(source_dirs) / sizeof(source_dirs[0])));
  for (ptrdiff_t i = 0; i < album_count; i++) {
    TEST_CHECK(str_eq(albums[i]->source_dir, source_dirs[i]));
    TEST_MSG("album %td: '%.*s'", i, STR_FMT(albums[i]->source_dir));
  }
  TEST_CHECK(albums[0]->sub_album_count == 8);
  for (ptrdiff_t i = 0; i < albums[0]->sub_album_count; i++) {
    TEST_CHECK(albums[0]->sub_albums[i] == albums[i + 1]);
  }
  TEST_CHECK(albums[1]->media_count == 1);
  TEST_CHECK(albums[1]->sub_album_count == 1);
  TEST_CHECK(albums[1]->sub_albums[0] == albums[9]);
  TEST_CHECK(albums[9]->parent == albums[1]);
  TEST_CHECK(albums[0]->item_count_total == 9);
}

// Source names remain literal while generated paths are slugged and display titles expose spaces.
static void test_build_skeleton_humanizes_album_title(void) {
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct Arena perm = test_arena();
  struct Str items[] = {S("/photos/John_White_Alexander/My_Photo.JPG")};
  const struct PathList paths = {.items = items, .count = 1, .capacity = 1};
  struct Album** albums = NULL;
  ptrdiff_t album_count = 0;
  struct MediaItem** media = NULL;
  ptrdiff_t media_count = 0;
  struct Log log = test_log();
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &perm, test_arena(), &albums,
                                           &album_count, &media, &media_count, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(album_count == 2);
  TEST_CHECK(str_eq(albums[1]->source_dir, S("John_White_Alexander")));
  TEST_CHECK(str_eq(albums[1]->slug_path, S("john-white-alexander")));
  TEST_CHECK(str_eq(albums[1]->title, S("John White Alexander")));
  TEST_CHECK(str_eq(albums[1]->url_path, S("john-white-alexander/index.html")));
  TEST_CHECK(str_eq(media[0]->title, S("My Photo.JPG")));
  TEST_CHECK(str_eq(media[0]->derivatives[0].url,
                    S("_fram/v2-s-120x120c-q70-m-520x360c-q80-l-792x594-q85-f1000/"
                      "john-white-alexander/my-photo-jpg-s.webp")));
}

// Slugging keeps a source named `_fram` separate from the internal `_fram` output namespace.
static void test_build_skeleton_slugs_internal_namespace_source(void) {
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct Arena perm = test_arena();
  struct Str items[] = {S("/photos/_FrAm/x.jpg")};
  const struct PathList paths = {.items = items, .count = 1, .capacity = 1};
  struct Album** albums = NULL;
  ptrdiff_t album_count = 0;
  struct MediaItem** media = NULL;
  ptrdiff_t media_count = 0;
  struct Log log = test_log();
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &perm, test_arena(), &albums,
                                           &album_count, &media, &media_count, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(album_count == 2);
  TEST_CHECK(media_count == 1);
  TEST_CHECK(str_eq(albums[1]->source_dir, S("_FrAm")));
  TEST_CHECK(str_eq(albums[1]->slug_path, S("fram")));
  TEST_CHECK(str_eq(albums[1]->url_path, S("fram/index.html")));
  TEST_CHECK(str_eq(media[0]->derivatives[0].url,
                    S("_fram/v2-s-120x120c-q70-m-520x360c-q80-l-792x594-q85-f1000/"
                      "fram/x-jpg-s.webp")));
}

// `media = "desc"` reverses each album's direct media at every level, and leaves every album's
// sub-albums ascending. An album with one item or none is unchanged. Each cover is the first item
// in display order: the last file by name, and for `z`, which has no media of its own, the cover of
// its first sub-album.
static void test_build_skeleton_reverses_media_when_descending(void) {
  struct Arena perm = test_arena();
  struct Album** albums = NULL;
  struct MediaItem** media = NULL;
  TEST_ASSERT(build_sort_fixture(GALLERY_SORT_ASC, GALLERY_SORT_DESC, &perm, &albums, &media) == 0);

  TEST_CHECK(albums[0]->media[0] == media[2]);
  TEST_CHECK(albums[0]->media[1] == media[1]);
  TEST_CHECK(albums[0]->media[2] == media[0]);
  TEST_CHECK(albums[1]->media[0] == media[4]);
  TEST_CHECK(albums[1]->media[1] == media[3]);
  TEST_CHECK(albums[4]->media_count == 1);
  TEST_CHECK(albums[4]->media[0] == media[7]);
  TEST_CHECK(albums[5]->media_count == 0);

  TEST_CHECK(albums[0]->sub_albums[0] == albums[1]);
  TEST_CHECK(albums[0]->sub_albums[1] == albums[4]);
  TEST_CHECK(albums[0]->sub_albums[2] == albums[5]);
  TEST_CHECK(albums[1]->sub_albums[0] == albums[2]);
  TEST_CHECK(albums[1]->sub_albums[1] == albums[3]);
  TEST_CHECK(albums[5]->sub_albums[0] == albums[6]);
  TEST_CHECK(albums[5]->sub_albums[1] == albums[7]);

  TEST_CHECK(albums[0]->cover == media[2]);
  TEST_CHECK(albums[1]->cover == media[4]);
  TEST_CHECK(albums[5]->cover == media[8]);
}

// `albums = "desc"` reverses each album's sub-albums at every level, and leaves every album's media
// ascending. An album with one sub-album or none is unchanged. `z` has no media of its own, so its
// cover comes from its first sub-album in display order, which is its last by name.
static void test_build_skeleton_reverses_sub_albums_when_descending(void) {
  struct Arena perm = test_arena();
  struct Album** albums = NULL;
  struct MediaItem** media = NULL;
  TEST_ASSERT(build_sort_fixture(GALLERY_SORT_DESC, GALLERY_SORT_ASC, &perm, &albums, &media) == 0);

  TEST_CHECK(albums[0]->sub_albums[0] == albums[5]);
  TEST_CHECK(albums[0]->sub_albums[1] == albums[4]);
  TEST_CHECK(albums[0]->sub_albums[2] == albums[1]);
  TEST_CHECK(albums[1]->sub_albums[0] == albums[3]);
  TEST_CHECK(albums[1]->sub_albums[1] == albums[2]);
  TEST_CHECK(albums[5]->sub_albums[0] == albums[7]);
  TEST_CHECK(albums[5]->sub_albums[1] == albums[6]);
  TEST_CHECK(albums[4]->sub_album_count == 0);

  TEST_CHECK(albums[0]->media[0] == media[0]);
  TEST_CHECK(albums[0]->media[1] == media[1]);
  TEST_CHECK(albums[0]->media[2] == media[2]);
  TEST_CHECK(albums[1]->media[0] == media[3]);
  TEST_CHECK(albums[1]->media[1] == media[4]);

  TEST_CHECK(albums[0]->cover == media[0]);
  TEST_CHECK(albums[5]->cover == media[9]);
}

// Under every combination of orders, the flat media array and the album array stay in source order.
// `find_album` binary-searches the albums, the cover pass relies on each album preceding its
// descendants, and the probe and derivative phases index the media.
static void test_build_skeleton_keeps_flat_arrays_in_source_order(void) {
  static const char* const album_dirs[] = {"", "x", "x/p", "x/q", "y", "z", "z/m", "z/n"};
  static const char* const media_paths[] = {
      "/photos/a.jpg",       "/photos/b.jpg",       "/photos/c.jpg",     "/photos/x/1.jpg",
      "/photos/x/2.jpg",     "/photos/x/p/9.jpg",   "/photos/x/q/8.jpg", "/photos/y/only.jpg",
      "/photos/z/m/one.jpg", "/photos/z/n/two.jpg",
  };
  static const enum GallerySortOrder orders[] = {GALLERY_SORT_ASC, GALLERY_SORT_DESC};

  for (ptrdiff_t a = 0; a < 2; a++) {
    for (ptrdiff_t m = 0; m < 2; m++) {
      struct Arena perm = test_arena();
      struct Album** albums = NULL;
      struct MediaItem** media = NULL;
      if (build_sort_fixture(orders[a], orders[m], &perm, &albums, &media) != 0) {
        continue;
      }
      for (ptrdiff_t i = 0; i < 8; i++) {
        TEST_CHECK(str_eq(albums[i]->source_dir, str_from_cstr(album_dirs[i])));
        TEST_MSG("albums %td, media %td, album %td", a, m, i);
      }
      for (ptrdiff_t i = 0; i < 10; i++) {
        TEST_CHECK(str_eq(media[i]->source_path, str_from_cstr(media_paths[i])));
        TEST_MSG("albums %td, media %td, media item %td", a, m, i);
      }
    }
  }
}

// Derivative suffixes are included when validating the portable per-component output limit. Each
// derivative's filename is the stem plus `-jpg-<name>.webp`, which puts it past the limit while the
// source name itself still fits, and so is the original copy's `-jpg.jpg`. The source is reported
// once, at its first output in planning order, and no skeleton is published. Without derivatives,
// the original copy is that first output.
static void test_build_skeleton_rejects_overlong_derivative_name(void) {
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct Arena perm = test_arena();
  struct Arena arena = test_arena();
  enum { STEM_LEN = FILENAME_LEN_MAX - 4 };
  const struct Str source = long_source_path('a', STEM_LEN, &arena);
  const struct Str stem = str_head(str_tail(source, sizeof("/photos/") - 1), STEM_LEN);
  struct Str items[] = {source};
  const struct PathList paths = {.items = items, .count = 1, .capacity = 1};
  struct Album** albums = NULL;
  ptrdiff_t album_count = 0;
  struct MediaItem** media = NULL;
  ptrdiff_t media_count = 0;
  struct Log log = test_log();
  TEST_CHECK(album_scanner_build_skeleton(&config, &paths, &perm, test_arena(), &albums,
                                          &album_count, &media, &media_count, &log) == -1);
  const int derivative_len = STEM_LEN + (int)(sizeof("-jpg-s.webp") - 1);
  const int original_len = STEM_LEN + (int)(sizeof("-jpg.jpg") - 1);
  TEST_CHECK(test_log_is(&log,
                         "output path segment for '%.*s' exceeds max filename length (%d bytes) "
                         "at %d bytes: '%.*s-jpg-s.webp'\n",
                         STR_FMT(source), FILENAME_LEN_MAX, derivative_len, STR_FMT(stem)));
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(albums == NULL);
  TEST_CHECK(media == NULL);

  config.derivative_count = 0;
  log = test_log();
  TEST_CHECK(album_scanner_build_skeleton(&config, &paths, &perm, test_arena(), &albums,
                                          &album_count, &media, &media_count, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path segment for '%.*s' exceeds max filename length (%d bytes) "
                         "at %d bytes: '%.*s-jpg.jpg'\n",
                         STR_FMT(source), FILENAME_LEN_MAX, original_len, STR_FMT(stem)));
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(albums == NULL);
  TEST_CHECK(media == NULL);
}

// Every source is planned even after one fails, so two sources whose derivative names are each one
// byte past the limit are both reported in one run, and no skeleton is published.
static void test_build_skeleton_reports_every_path_limit_failure(void) {
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  const struct GalleryDerivative configured = {
      .name = S("s"), .width_px = 120, .height_px = 120, .quality = 70, .is_crop = true};
  config.derivatives = &configured;
  config.derivative_count = 1;
  struct Arena perm = test_arena();
  const struct Arena perm_before = perm;
  struct Arena arena = test_arena();
  // `-jpg-s.webp` takes the stem one byte past the limit, and the original's `-jpg.jpg` fits.
  enum { STEM_LEN = FILENAME_LEN_MAX - 9 };
  const struct Str first = long_source_path('a', STEM_LEN, &arena);
  const struct Str second = long_source_path('b', STEM_LEN, &arena);
  const struct Str first_stem = str_head(str_tail(first, sizeof("/photos/") - 1), STEM_LEN);
  const struct Str second_stem = str_head(str_tail(second, sizeof("/photos/") - 1), STEM_LEN);
  struct Str items[] = {first, second};
  const struct PathList paths = {.items = items, .count = 2, .capacity = 2};
  struct Album** albums = NULL;
  ptrdiff_t album_count = 0;
  struct MediaItem** media = NULL;
  ptrdiff_t media_count = 0;
  struct Log log = test_log();
  TEST_CHECK(album_scanner_build_skeleton(&config, &paths, &perm, test_arena(), &albums,
                                          &album_count, &media, &media_count, &log) == -1);
  const int segment_len = STEM_LEN + (int)(sizeof("-jpg-s.webp") - 1);
  TEST_CHECK(test_log_is(&log,
                         "output path segment for '%.*s' exceeds max filename length (%d bytes) "
                         "at %d bytes: '%.*s-jpg-s.webp'\n"
                         "output path segment for '%.*s' exceeds max filename length (%d bytes) "
                         "at %d bytes: '%.*s-jpg-s.webp'\n",
                         STR_FMT(first), FILENAME_LEN_MAX, segment_len, STR_FMT(first_stem),
                         STR_FMT(second), FILENAME_LEN_MAX, segment_len, STR_FMT(second_stem)));
  TEST_CHECK(log.errors == 2);
  TEST_CHECK(albums == NULL);
  TEST_CHECK(media == NULL);
  TEST_CHECK(perm.beg == perm_before.beg);
}

// An album page path past the whole-path limit is rejected with the limit and the measured length
// ahead of the path. Six 200-byte directories make the deepest album's page path 1216 bytes long:
// `6 * 200 + 5` bytes of directories and separators, then `/index.html`. Every shallower page still
// fits. The album is reported once. Its sources and its sub-album extend its path, so none of them
// adds a line of its own.
static void test_build_skeleton_rejects_overlong_album_path(void) {
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct Arena perm = test_arena();
  struct Arena arena = test_arena();
  enum { SEGMENT_LEN = 200, DEPTH = 6 };
  char source[sizeof("/photos") - 1 + DEPTH * (SEGMENT_LEN + 1) + sizeof("/x.jpg")];
  size_t source_len = sizeof("/photos") - 1;
  memcpy(source, "/photos", source_len);
  for (size_t i = 0; i < (size_t)DEPTH; i++) {
    source[source_len++] = '/';
    memset(source + source_len, 'a', SEGMENT_LEN);
    source_len += SEGMENT_LEN;
  }
  const struct Str album_source = {source, (ptrdiff_t)source_len};
  // The paths are in the walk's byte order, so the sub-album's source comes first.
  struct Str items[] = {
      str_concat(album_source, S("/sub/z.jpg"), &arena),
      str_concat(album_source, S("/x.jpg"), &arena),
      str_concat(album_source, S("/y.jpg"), &arena),
  };
  const struct PathList paths = {.items = items, .count = 3, .capacity = 3};
  struct Album** albums = NULL;
  ptrdiff_t album_count = 0;
  struct MediaItem** media = NULL;
  ptrdiff_t media_count = 0;
  struct Log log = test_log();
  TEST_CHECK(album_scanner_build_skeleton(&config, &paths, &perm, test_arena(), &albums,
                                          &album_count, &media, &media_count, &log) == -1);

  // The album directory, below `/photos/`, is also the album's slug path, because `a` slugs to
  // itself.
  const struct Str album_dir = {source + sizeof("/photos/") - 1,
                                (ptrdiff_t)(source_len - (sizeof("/photos/") - 1))};
  const struct Str page = path_join(album_dir, S("index.html"), &arena);
  struct Buf expected = {.arena = &arena};
  buf_printf(&expected,
             "output path for '%.*s' exceeds max output path length (%d bytes) at %td bytes: "
             "'%.*s'\n",
             STR_FMT(album_dir), OUTPUT_PATH_RELATIVE_LEN_MAX, page.len, STR_FMT(page));
  const struct Str expected_text = buf_str(&expected);
  TEST_CHECK(test_log_is(&log, "%.*s", STR_FMT(expected_text)));
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(albums == NULL);
  TEST_CHECK(media == NULL);
}

// Image probing reads EXIF before computing presented and fitted dimensions.
static void test_probe_media_orients_image_dimensions(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  const struct Str image_path = S("photos/oriented.jpg");
  struct Str jpeg = {0};
  struct Str oriented = {0};
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct MediaDerivative derivatives[3] = {0};
  struct MediaItem item = {.source_path = image_path,
                           .kind = MEDIA_KIND_IMAGE,
                           .derivatives = derivatives,
                           .derivative_count = 3};
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct JobArenas* worker_arenas = NULL;
  struct Log log = test_log();
  if (!TEST_CHECK(test_jpeg_encode(2, 3, &arena, &jpeg) == 0)) {
    return;
  }
  if (!TEST_CHECK(add_right_top_exif(jpeg, &arena, &oriented) == 0)) {
    return;
  }
  os_memory_write_bytes(os, image_path, oriented);

  worker_arenas = test_job_arenas(2, &arena);
  TEST_CHECK(album_scanner_probe_media(&config, media, 1, frames, 2, worker_arenas, false, os,
                                       arena, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(item.orientation == IMAGE_ORIENTATION_RIGHT_TOP);
  TEST_CHECK(item.width_px == 3);
  TEST_CHECK(item.height_px == 2);
  TEST_CHECK(item.derivatives[0].width_px == 2);
  TEST_CHECK(item.derivatives[0].height_px == 2);
  TEST_CHECK(item.derivatives[1].width_px == 2);
  TEST_CHECK(item.derivatives[1].height_px == 2);
  TEST_CHECK(item.derivatives[2].width_px == 3);
  TEST_CHECK(item.derivatives[2].height_px == 2);
  TEST_CHECK(!frames[0].is_created);
}

// A large cropped derivative reaches its configured dimensions despite integer crop geometry.
static void test_probe_media_sizes_cropped_derivative_exactly(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  const struct Str image_path = S("photos/wide.jpg");
  struct Str jpeg = {0};
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  const struct GalleryDerivative configured = {
      .name = S("m"), .width_px = 520, .height_px = 360, .quality = 80, .is_crop = true};
  config.derivatives = &configured;
  config.derivative_count = 1;
  struct MediaDerivative derivative = {0};
  struct MediaItem item = {.source_path = image_path,
                           .kind = MEDIA_KIND_IMAGE,
                           .derivatives = &derivative,
                           .derivative_count = 1};
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct JobArenas* worker_arenas = NULL;
  struct Log log = test_log();
  if (!TEST_CHECK(test_jpeg_encode(1200, 721, &arena, &jpeg) == 0)) {
    return;
  }
  os_memory_write_bytes(os, image_path, jpeg);

  worker_arenas = test_job_arenas(1, &arena);
  TEST_CHECK(album_scanner_probe_media(&config, media, 1, frames, 1, worker_arenas, false, os,
                                       arena, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(derivative.width_px == 520);
  TEST_CHECK(derivative.height_px == 360);
}

// Probe failures are associated with their source and leave image frame slots empty.
static void test_probe_media_collects_failure(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  const struct Str path = S("photos/bad.jpg");
  os_memory_write(os, "photos/bad.jpg", "not jpeg");
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct MediaItem item = {.source_path = path, .kind = MEDIA_KIND_IMAGE};
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  struct JobArenas* worker_arenas = test_job_arenas(1, &arena);
  TEST_CHECK(album_scanner_probe_media(&config, media, 1, frames, 1, worker_arenas, false, os,
                                       arena, &log) == -1);
  const struct Str errors = buf_str(&log.buf);
  TEST_CHECK(str_startswith(errors, S("failed to probe media: failed to probe JPEG: ")));
  TEST_CHECK(
      str_endswith(errors, str_concat(S(" ('"), str_concat(path, S("')\n"), &arena), &arena)));
  TEST_MSG("log: '%.*s'", STR_FMT(errors));
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(!frames[0].is_created);
}

// A source larger than `IMAGE_INPUT_LEN_MAX`, the media read limit, fails its probe at the read,
// from its size, naming the limit and the size, rather than being loaded whole. The file is sparse,
// so the fixture costs no memory.
static void test_probe_media_rejects_oversize_source(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  const ptrdiff_t source_len = (ptrdiff_t)IMAGE_INPUT_LEN_MAX + 1;
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct MediaItem item = {.source_path = S("photos/huge.jpg"), .kind = MEDIA_KIND_IMAGE};
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  os_memory_write(os, "photos/huge.jpg", "");
  os_memory_truncate(os, "photos/huge.jpg", source_len);
  struct JobArenas* worker_arenas = test_job_arenas(1, &arena);
  TEST_CHECK(album_scanner_probe_media(&config, media, 1, frames, 1, worker_arenas, false, os,
                                       arena, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: exceeds max file size (%d bytes) at %td bytes "
                         "('photos/huge.jpg')\n",
                         IMAGE_INPUT_LEN_MAX, source_len));
}

TEST_LIST = {
    {"build skeleton synthesizes empty root", test_build_skeleton_synthesizes_empty_root},
    {"build skeleton derives nested gallery", test_build_skeleton_derives_nested_gallery},
    {"build skeleton sorts late ancestors", test_build_skeleton_sorts_late_ancestors},
    {"build skeleton inserts late album before siblings",
     test_build_skeleton_inserts_late_album_before_siblings},
    {"build skeleton humanizes album title", test_build_skeleton_humanizes_album_title},
    {"build skeleton slugs internal namespace source",
     test_build_skeleton_slugs_internal_namespace_source},
    {"build skeleton reverses media when descending",
     test_build_skeleton_reverses_media_when_descending},
    {"build skeleton reverses sub albums when descending",
     test_build_skeleton_reverses_sub_albums_when_descending},
    {"build skeleton keeps flat arrays in source order",
     test_build_skeleton_keeps_flat_arrays_in_source_order},
    {"build skeleton rejects overlong derivative name",
     test_build_skeleton_rejects_overlong_derivative_name},
    {"build skeleton reports every path limit failure",
     test_build_skeleton_reports_every_path_limit_failure},
    {"build skeleton rejects overlong album path", test_build_skeleton_rejects_overlong_album_path},
    {"probe media orients image dimensions", test_probe_media_orients_image_dimensions},
    {"probe media sizes cropped derivative exactly",
     test_probe_media_sizes_cropped_derivative_exactly},
    {"probe media collects failure", test_probe_media_collects_failure},
    {"probe media rejects oversize source", test_probe_media_rejects_oversize_source},
    {NULL, NULL},
};
