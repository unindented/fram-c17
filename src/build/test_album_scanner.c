#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build/album_scanner.h"
#include "build/test_jpeg.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/text.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/media_item.h"
#include "formats/image.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few hundred bytes. */
enum { TEST_FILE_LEN_MAX = 1024 * 1024 };

/**
 * @brief Initializes the minimal fixed configuration the scanner tests share.
 *
 * Every string is a borrowed literal, so the caller releases nothing beyond what
 * `gallery_config_init` itself requires.
 *
 * @param config Configuration to initialize. Must not be `NULL`.
 */
static void init_config(struct GalleryConfig* config) {
  gallery_config_init(config);
  config->title = "Gallery";
  config->author = "Author";
  config->input_dir = "/photos";
  config->output_dir = "/public";
}

/**
 * @brief Inserts an EXIF segment carrying the right-top orientation into a JPEG.
 *
 * The segment goes directly after the SOI marker, ahead of the encoder's own segments.
 *
 * @param jpeg         JPEG bytes to copy. Must not be `NULL`.
 * @param jpeg_len     Number of bytes at `jpeg`, at least 2.
 * @param data_len_out Receives the length of the returned JPEG in bytes. Must not be `NULL`.
 * @return The oriented JPEG the caller must `free`, or `NULL` on a short input or allocation
 *         failure.
 */
static unsigned char* add_right_top_exif(const unsigned char* jpeg,
                                         size_t jpeg_len,
                                         size_t* data_len_out) {
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
  if (jpeg_len < 2 || jpeg_len > SIZE_MAX - sizeof(segment)) {
    return NULL;
  }
  unsigned char* data = malloc(jpeg_len + sizeof(segment));
  if (data == NULL) {
    return NULL;
  }
  memcpy(data, jpeg, 2);
  memcpy(data + 2, segment, sizeof(segment));
  memcpy(data + 2 + sizeof(segment), jpeg + 2, jpeg_len - 2);
  *data_len_out = jpeg_len + sizeof(segment);
  return data;
}

/**
 * @brief Writes fake `ffprobe` and `ffmpeg` scripts used to test seek clamping and retained frames.
 *
 * The probe reports a 0.4-second duration. The extractor logs its `-ss` argument to
 * `$FRAM_TEST_LOG` and copies `$FRAM_TEST_FRAME` to its output path.
 *
 * @param root_dir Directory that receives both executable scripts. Must not be `NULL`.
 * @param arena    Arena that owns the script paths. Must not be `NULL`.
 * @return `0` on success, or `-1` after failing the current test check.
 */
static int write_fake_video_tools(const char* root_dir, struct Arena* arena) {
  static const char probe_script[] = "#!/bin/sh\nprintf '0.400\\n'\n";
  static const char frame_script[] =
      "#!/bin/sh\n"
      "previous=\n"
      "last=\n"
      "for arg do\n"
      "  if [ \"$previous\" = -ss ]; then printf '%s' \"$arg\" > \"$FRAM_TEST_LOG\"; fi\n"
      "  previous=$arg\n"
      "  last=$arg\n"
      "done\n"
      "/bin/cp \"$FRAM_TEST_FRAME\" \"$last\"\n";
  char* probe_path = path_join(root_dir, "ffprobe", arena);
  char* frame_path = path_join(root_dir, "ffmpeg", arena);
  if (probe_path == NULL || frame_path == NULL ||
      fs_write_file(probe_path, probe_script, sizeof(probe_script) - 1, NULL, 0) != 0 ||
      fs_write_file(frame_path, frame_script, sizeof(frame_script) - 1, NULL, 0) != 0 ||
      chmod(probe_path, 0700) != 0 || chmod(frame_path, 0700) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  return 0;
}

/**
 * @brief Restores one environment variable from a saved value.
 *
 * @param name  Environment variable to restore. Must not be `NULL`.
 * @param saved Value saved before the test, which this releases with `free`, or `NULL` to unset the
 *              variable.
 */
static void restore_env(const char* name, char* saved) {
  if (saved == NULL) {
    (void)unsetenv(name);
  } else {
    (void)setenv(name, saved, 1);
    free(saved);
  }
}

// An empty source tree still produces the root page and no flat media allocation.
static void test_build_skeleton_synthesizes_empty_root(void) {
  struct GalleryConfig config;
  init_config(&config);
  const struct PathList paths = {0};
  struct Album** albums = NULL;
  size_t album_count = 0;
  struct MediaItem** media = NULL;
  size_t media_count = 0;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &albums, &album_count, &media,
                                           &media_count, err, sizeof(err)) == 0);
  TEST_CHECK(album_count == 1);
  TEST_CHECK(media_count == 0);
  TEST_CHECK(media == NULL);
  TEST_CHECK(strcmp(albums[0]->source_dir, "") == 0);
  TEST_CHECK(strcmp(albums[0]->title, "Gallery") == 0);
  TEST_CHECK(strcmp(albums[0]->url_path, "index.html") == 0);
  TEST_CHECK(strcmp(albums[0]->output_path, "/public/index.html") == 0);
  TEST_CHECK(strcmp(albums[0]->path_to_root, "") == 0);
  TEST_CHECK(albums[0]->cover == NULL);
  album_scanner_free_skeleton(albums, album_count, media);
  gallery_config_free(&config);
}

// Sorted paths form a deterministic recursive tree with recipe paths, URLs, covers, and totals.
static void test_build_skeleton_derives_nested_gallery(void) {
  struct GalleryConfig config;
  init_config(&config);
  char* items[] = {
      "/photos/2024/Japan/a #.JPG",
      "/photos/2024/Japan/clip.MP4",
      "/photos/2024/root.jpeg",
      "/photos/z.jpg",
  };
  const struct PathList paths = {.items = items, .count = 4, .capacity = 4};
  struct Album** albums = NULL;
  size_t album_count = 0;
  struct MediaItem** media = NULL;
  size_t media_count = 0;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &albums, &album_count, &media,
                                           &media_count, err, sizeof(err)) == 0);
  TEST_CHECK(album_count == 3);
  TEST_CHECK(media_count == 4);
  TEST_CHECK(strcmp(albums[0]->source_dir, "") == 0);
  TEST_CHECK(strcmp(albums[1]->source_dir, "2024") == 0);
  TEST_CHECK(strcmp(albums[2]->source_dir, "2024/Japan") == 0);
  TEST_CHECK(strcmp(albums[2]->slug_path, "2024/japan") == 0);
  TEST_CHECK(albums[1]->parent == albums[0]);
  TEST_CHECK(albums[2]->parent == albums[1]);
  TEST_CHECK(albums[0]->sub_albums[0] == albums[1]);
  TEST_CHECK(albums[1]->sub_albums[0] == albums[2]);
  TEST_CHECK(strcmp(albums[2]->path_to_root, "../../") == 0);
  TEST_CHECK(strcmp(albums[2]->url_path, "2024/japan/index.html") == 0);

  TEST_CHECK(strcmp(media[0]->title, "a #.JPG") == 0);
  TEST_CHECK(media[0]->kind == MEDIA_KIND_IMAGE);
  TEST_CHECK(media[1]->kind == MEDIA_KIND_VIDEO);
  TEST_CHECK(media[0]->derivative_count == 3);
  TEST_CHECK(strcmp(media[0]->derivatives[0].name, "s") == 0);
  TEST_CHECK(strcmp(media[0]->derivatives[0].url,
                    "_fram/v1-s-120x120c-q70-m-520x360c-q80-l-792x594-q85-f1000/"
                    "2024/japan/a-jpg-s.jpg") == 0);
  TEST_CHECK(strcmp(media[0]->derivatives[0].path,
                    "/public/_fram/v1-s-120x120c-q70-m-520x360c-q80-l-792x594-q85-f1000/"
                    "2024/japan/a-jpg-s.jpg") == 0);
  TEST_CHECK(strcmp(media[1]->original_url, "_fram/originals/2024/japan/clip-mp4.mp4") == 0);
  TEST_CHECK(albums[2]->media_count == 2);
  TEST_CHECK(albums[2]->media[0] == media[0]);
  TEST_CHECK(albums[2]->media[1] == media[1]);

  TEST_CHECK(albums[2]->item_count_total == 2);
  TEST_CHECK(albums[1]->item_count_total == 3);
  TEST_CHECK(albums[0]->item_count_total == 4);
  TEST_CHECK(albums[2]->cover == media[0]);
  TEST_CHECK(albums[1]->cover == media[2]);
  TEST_CHECK(albums[0]->cover == media[3]);

  album_scanner_free_skeleton(albums, album_count, media);
  gallery_config_free(&config);
}

// Source-file ordering can discover `a-c` before ancestor `a`. The album index remains sorted so
// binary lookup and reverse child aggregation stay valid.
static void test_build_skeleton_sorts_late_ancestors(void) {
  struct GalleryConfig config;
  init_config(&config);
  char* items[] = {
      "/photos/a-c/file.jpg",
      "/photos/a/b/file.jpg",
  };
  const struct PathList paths = {.items = items, .count = 2, .capacity = 2};
  struct Album** albums = NULL;
  size_t album_count = 0;
  struct MediaItem** media = NULL;
  size_t media_count = 0;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &albums, &album_count, &media,
                                           &media_count, err, sizeof(err)) == 0);
  TEST_CHECK(album_count == 4);
  TEST_CHECK(strcmp(albums[0]->source_dir, "") == 0);
  TEST_CHECK(strcmp(albums[1]->source_dir, "a") == 0);
  TEST_CHECK(strcmp(albums[2]->source_dir, "a-c") == 0);
  TEST_CHECK(strcmp(albums[3]->source_dir, "a/b") == 0);
  TEST_CHECK(albums[3]->parent == albums[1]);
  TEST_CHECK(albums[0]->sub_album_count == 2);
  TEST_CHECK(albums[0]->sub_albums[0] == albums[1]);
  TEST_CHECK(albums[0]->sub_albums[1] == albums[2]);
  TEST_CHECK(albums[1]->sub_albums[0] == albums[3]);
  TEST_CHECK(albums[0]->item_count_total == 2);

  album_scanner_free_skeleton(albums, album_count, media);
  gallery_config_free(&config);
}

// An album discovered after many siblings that sort after it is inserted at its sorted place, far
// from the end of the index, and every later lookup still finds it and its siblings.
static void test_build_skeleton_inserts_late_album_before_siblings(void) {
  struct GalleryConfig config;
  init_config(&config);
  char* items[] = {
      "/photos/a-1/file.jpg", "/photos/a-2/file.jpg", "/photos/a-3/file.jpg",
      "/photos/a-4/file.jpg", "/photos/a-5/file.jpg", "/photos/a-6/file.jpg",
      "/photos/a-7/file.jpg", "/photos/a/b/file.jpg", "/photos/a/file.jpg",
  };
  const struct PathList paths = {.items = items, .count = 9, .capacity = 9};
  struct Album** albums = NULL;
  size_t album_count = 0;
  struct MediaItem** media = NULL;
  size_t media_count = 0;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &albums, &album_count, &media,
                                           &media_count, err, sizeof(err)) == 0);
  static const char* const source_dirs[] = {"",    "a",   "a-1", "a-2", "a-3",
                                            "a-4", "a-5", "a-6", "a-7", "a/b"};
  TEST_ASSERT(album_count == sizeof(source_dirs) / sizeof(source_dirs[0]));
  for (size_t i = 0; i < album_count; i++) {
    TEST_CHECK(strcmp(albums[i]->source_dir, source_dirs[i]) == 0);
    TEST_MSG("album %zu: '%s'", i, albums[i]->source_dir);
  }
  TEST_CHECK(albums[0]->sub_album_count == 8);
  for (size_t i = 0; i < albums[0]->sub_album_count; i++) {
    TEST_CHECK(albums[0]->sub_albums[i] == albums[i + 1]);
  }
  TEST_CHECK(albums[1]->media_count == 1);
  TEST_CHECK(albums[1]->sub_album_count == 1);
  TEST_CHECK(albums[1]->sub_albums[0] == albums[9]);
  TEST_CHECK(albums[9]->parent == albums[1]);
  TEST_CHECK(albums[0]->item_count_total == 9);

  album_scanner_free_skeleton(albums, album_count, media);
  gallery_config_free(&config);
}

// Source names remain literal while generated paths are slugged and display titles expose spaces.
static void test_build_skeleton_humanizes_album_title(void) {
  struct GalleryConfig config;
  init_config(&config);
  char* items[] = {"/photos/John_White_Alexander/My_Photo.JPG"};
  const struct PathList paths = {.items = items, .count = 1, .capacity = 1};
  struct Album** albums = NULL;
  size_t album_count = 0;
  struct MediaItem** media = NULL;
  size_t media_count = 0;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &albums, &album_count, &media,
                                           &media_count, err, sizeof(err)) == 0);
  TEST_CHECK(album_count == 2);
  TEST_CHECK(strcmp(albums[1]->source_dir, "John_White_Alexander") == 0);
  TEST_CHECK(strcmp(albums[1]->slug_path, "john-white-alexander") == 0);
  TEST_CHECK(strcmp(albums[1]->title, "John White Alexander") == 0);
  TEST_CHECK(strcmp(albums[1]->url_path, "john-white-alexander/index.html") == 0);
  TEST_CHECK(strcmp(media[0]->title, "My Photo.JPG") == 0);
  TEST_CHECK(strcmp(media[0]->derivatives[0].url,
                    "_fram/v1-s-120x120c-q70-m-520x360c-q80-l-792x594-q85-f1000/"
                    "john-white-alexander/my-photo-jpg-s.jpg") == 0);

  album_scanner_free_skeleton(albums, album_count, media);
  gallery_config_free(&config);
}

// Slugging keeps a source named `_fram` separate from the internal `_fram` output namespace.
static void test_build_skeleton_slugs_internal_namespace_source(void) {
  struct GalleryConfig config;
  init_config(&config);
  char* items[] = {"/photos/_FrAm/x.jpg"};
  const struct PathList paths = {.items = items, .count = 1, .capacity = 1};
  struct Album** albums = NULL;
  size_t album_count = 0;
  struct MediaItem** media = NULL;
  size_t media_count = 0;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_ASSERT(album_scanner_build_skeleton(&config, &paths, &albums, &album_count, &media,
                                           &media_count, err, sizeof(err)) == 0);
  TEST_CHECK(album_count == 2);
  TEST_CHECK(media_count == 1);
  TEST_CHECK(strcmp(albums[1]->source_dir, "_FrAm") == 0);
  TEST_CHECK(strcmp(albums[1]->slug_path, "fram") == 0);
  TEST_CHECK(strcmp(albums[1]->url_path, "fram/index.html") == 0);
  TEST_CHECK(strcmp(media[0]->derivatives[0].url,
                    "_fram/v1-s-120x120c-q70-m-520x360c-q80-l-792x594-q85-f1000/"
                    "fram/x-jpg-s.jpg") == 0);
  album_scanner_free_skeleton(albums, album_count, media);
  gallery_config_free(&config);
}

// Source paths that are not in strictly ascending order are refused, naming the first path out of
// order. The album index and the flat media array both rely on that order, so a duplicate is
// refused as well as a path that sorts before its predecessor. `cmd_build` cannot produce either
// form because `fs_list_files_with_suffixes` returns sorted, distinct paths. A caller that supplies
// a source list can produce them, so this function checks the order.
static void test_build_skeleton_rejects_unsorted_sources(void) {
  static const char* const cases[][3] = {
      {"/photos/b.jpg", "/photos/a.jpg", "source paths are not strictly sorted: '/photos/a.jpg'"},
      {"/photos/a.jpg", "/photos/a.jpg", "source paths are not strictly sorted: '/photos/a.jpg'"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct GalleryConfig config;
    init_config(&config);
    char* items[] = {(char*)cases[i][0], (char*)cases[i][1]};
    const struct PathList paths = {.items = items, .count = 2, .capacity = 2};
    struct Album** albums = NULL;
    size_t album_count = 0;
    struct MediaItem** media = NULL;
    size_t media_count = 0;
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(album_scanner_build_skeleton(&config, &paths, &albums, &album_count, &media,
                                            &media_count, err, sizeof(err)) == -1);
    // Compared whole, so a message that merely mentions the source path cannot pass for this one.
    TEST_CHECK(strcmp(err, cases[i][2]) == 0);
    TEST_MSG("case %zu: err: %s", i, err);
    TEST_CHECK(albums == NULL);
    TEST_CHECK(media == NULL);
    gallery_config_free(&config);
  }
}

// A source outside `<input_dir>/` is refused. Otherwise, the complete path would become its album
// directory and add the input directory name to every output path. The test uses two path forms.
// One shares no prefix with `input_dir`. The other shares the prefix but has no `/` boundary after
// it. A prefix-only test would accept the second form and produce a bogus album. `cmd_build` cannot
// produce either form because its walk starts at `input_dir`. A caller that supplies a source list
// can produce them, so this function checks the prefix.
static void test_build_skeleton_rejects_source_outside_input_dir(void) {
  static const char* const cases[][2] = {
      {"/elsewhere/one.jpg",
       "media source must be under the configured 'input_dir': '/elsewhere/one.jpg'"},
      {"/photosx/one.jpg",
       "media source must be under the configured 'input_dir': '/photosx/one.jpg'"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct GalleryConfig config;
    init_config(&config);
    char* items[] = {(char*)cases[i][0]};
    const struct PathList paths = {.items = items, .count = 1, .capacity = 1};
    struct Album** albums = NULL;
    size_t album_count = 0;
    struct MediaItem** media = NULL;
    size_t media_count = 0;
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(album_scanner_build_skeleton(&config, &paths, &albums, &album_count, &media,
                                            &media_count, err, sizeof(err)) == -1);
    // Compared whole, so a message that merely mentions the source path cannot pass for this one.
    TEST_CHECK(strcmp(err, cases[i][1]) == 0);
    TEST_MSG("case %zu: err: %s", i, err);
    // No skeleton was published, so nothing downstream can read an album derived from a bad path.
    TEST_CHECK(albums == NULL);
    TEST_CHECK(media == NULL);
    gallery_config_free(&config);
  }
}

// Derivative suffixes are included when validating the portable per-component output limit. The
// first derivative's filename is the stem plus `-jpg-s.jpg`, which puts it past the limit while the
// source name itself still fits. The diagnostic reports the limit and the measured segment length
// ahead of the source and the offending segment, which values this long push past the end of the
// buffer.
static void test_build_skeleton_rejects_overlong_derivative_name(void) {
  struct GalleryConfig config;
  init_config(&config);
  enum { STEM_LEN = FILENAME_LEN_MAX - 4 };
  char source[sizeof("/photos/") - 1 + STEM_LEN + sizeof(".jpg")];
  memcpy(source, "/photos/", sizeof("/photos/") - 1);
  memset(source + sizeof("/photos/") - 1, 'a', STEM_LEN);
  memcpy(source + sizeof("/photos/") - 1 + STEM_LEN, ".jpg", sizeof(".jpg"));
  char* items[] = {source};
  const struct PathList paths = {.items = items, .count = 1, .capacity = 1};
  struct Album** albums = NULL;
  size_t album_count = 0;
  struct MediaItem** media = NULL;
  size_t media_count = 0;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(album_scanner_build_skeleton(&config, &paths, &albums, &album_count, &media,
                                          &media_count, err, sizeof(err)) == -1);
  // The full message is longer than the diagnostic buffer, so the output path trails off the end.
  // Everything ahead of the truncation marker, including both lengths, has to match exactly. The
  // `...` is spelled out because `TRUNCATION_MARKER` is file-local to `core/error.c`. Changing it
  // there must update this test.
  char expected[2 * ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(
      expected, sizeof(expected),
      "output path segment exceeds max filename length (%zu bytes) at %zu bytes (for '%s'): "
      "'%.*s-jpg-s.jpg'",
      (size_t)FILENAME_LEN_MAX, (size_t)STEM_LEN + (sizeof("-jpg-s.jpg") - 1), source,
      (int)STEM_LEN, source + sizeof("/photos/") - 1);
  TEST_CHECK(expected_len >= (int)sizeof(err) && (size_t)expected_len < sizeof(expected));
  const size_t err_actual_len = strlen(err);
  TEST_CHECK(err_actual_len == sizeof(err) - 1);
  TEST_CHECK(strncmp(err, expected, err_actual_len - 3) == 0);
  TEST_CHECK(strcmp(err + err_actual_len - 3, "...") == 0);
  TEST_MSG("err: %s", err);
  TEST_CHECK(albums == NULL);
  TEST_CHECK(media == NULL);
  gallery_config_free(&config);
}

// An album page path past the whole-path limit is rejected with the limit and the measured length
// ahead of the path. The path is longer than the diagnostic buffer, so leading with it would push
// both numbers out of the message. Six 200-byte directories make the deepest album's page
// `6 * 200 + 5` separator bytes plus `/index.html`, while every shallower page still fits.
static void test_build_skeleton_rejects_overlong_album_path(void) {
  struct GalleryConfig config;
  init_config(&config);
  enum { SEGMENT_LEN = 200, DEPTH = 6 };
  char source[sizeof("/photos") - 1 + DEPTH * (SEGMENT_LEN + 1) + sizeof("/x.jpg")];
  size_t source_len = sizeof("/photos") - 1;
  memcpy(source, "/photos", source_len);
  for (size_t i = 0; i < (size_t)DEPTH; i++) {
    source[source_len++] = '/';
    memset(source + source_len, 'a', SEGMENT_LEN);
    source_len += SEGMENT_LEN;
  }
  memcpy(source + source_len, "/x.jpg", sizeof("/x.jpg"));
  char* items[] = {source};
  const struct PathList paths = {.items = items, .count = 1, .capacity = 1};
  struct Album** albums = NULL;
  size_t album_count = 0;
  struct MediaItem** media = NULL;
  size_t media_count = 0;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(album_scanner_build_skeleton(&config, &paths, &albums, &album_count, &media,
                                          &media_count, err, sizeof(err)) == -1);
  char expected_head[128];
  const int expected_head_len =
      snprintf(expected_head, sizeof(expected_head),
               "output path exceeds max output path length (%zu bytes) at %zu bytes (for '",
               (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX,
               (size_t)DEPTH * SEGMENT_LEN + (size_t)(DEPTH - 1) + (sizeof("/index.html") - 1));
  TEST_CHECK(expected_head_len > 0 && (size_t)expected_head_len < sizeof(expected_head));
  TEST_CHECK(strncmp(err, expected_head, (size_t)expected_head_len) == 0);
  TEST_MSG("err: %s", err);
  TEST_CHECK(albums == NULL);
  TEST_CHECK(media == NULL);
  gallery_config_free(&config);
}

// Image probing reads EXIF before computing presented and fitted dimensions.
static void test_probe_media_orients_image_dimensions(void) {
  char root_dir_template[] = "/tmp/fram-scanner-image.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena arena;
  arena_init(&arena);
  char* image_path = path_join(root_dir, "oriented.jpg", &arena);
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  size_t oriented_len = 0;
  unsigned char* oriented = NULL;
  struct GalleryConfig config;
  init_config(&config);
  struct MediaDerivative derivatives[3] = {0};
  struct MediaItem item = {.source_path = image_path,
                           .kind = MEDIA_KIND_IMAGE,
                           .derivatives = derivatives,
                           .derivative_count = 3};
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(test_jpeg_encode(2, 3, &jpeg, &jpeg_len) == 0)) {
    goto cleanup;
  }
  oriented = add_right_top_exif(jpeg, jpeg_len, &oriented_len);
  if (!TEST_CHECK(oriented != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(fs_write_file(image_path, oriented, oriented_len, NULL, 0) == 0);

  TEST_CHECK(album_scanner_probe_media(&config, media, 1, frames, 2, false, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_CHECK(item.orientation == IMAGE_ORIENTATION_RIGHT_TOP);
  TEST_CHECK(item.width_px == 3);
  TEST_CHECK(item.height_px == 2);
  TEST_CHECK(item.derivatives[0].width_px == 2);
  TEST_CHECK(item.derivatives[0].height_px == 2);
  TEST_CHECK(item.derivatives[1].width_px == 2);
  TEST_CHECK(item.derivatives[1].height_px == 2);
  TEST_CHECK(item.derivatives[2].width_px == 3);
  TEST_CHECK(item.derivatives[2].height_px == 2);
  TEST_CHECK(frames[0] == NULL);

cleanup:
  free(oriented);
  free(jpeg);
  string_buffer_free(&error_buffer);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A large cropped derivative reaches its configured dimensions despite integer crop geometry.
static void test_probe_media_sizes_cropped_derivative_exactly(void) {
  char root_dir_template[] = "/tmp/fram-scanner-crop.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena arena;
  arena_init(&arena);
  char* image_path = path_join(root_dir, "wide.jpg", &arena);
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  struct GalleryConfig config;
  init_config(&config);
  const struct GalleryDerivative configured = {
      .name = "m", .width_px = 520, .height_px = 360, .quality = 80, .is_crop = true};
  config.derivatives = &configured;
  config.derivative_count = 1;
  struct MediaDerivative derivative = {0};
  struct MediaItem item = {.source_path = image_path,
                           .kind = MEDIA_KIND_IMAGE,
                           .derivatives = &derivative,
                           .derivative_count = 1};
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(test_jpeg_encode(1200, 721, &jpeg, &jpeg_len) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(fs_write_file(image_path, jpeg, jpeg_len, NULL, 0) == 0);

  TEST_CHECK(album_scanner_probe_media(&config, media, 1, frames, 1, false, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_CHECK(derivative.width_px == 520);
  TEST_CHECK(derivative.height_px == 360);

cleanup:
  free(jpeg);
  string_buffer_free(&error_buffer);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A short video seeks to its midpoint and leaves one caller-owned auto-rotated frame path.
static void test_probe_media_retains_clamped_video_frame(void) {
  char root_dir_template[] = "/tmp/fram-scanner-video.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena arena;
  arena_init(&arena);
  char* frame_fixture = path_join(root_dir, "frame.jpg", &arena);
  char* log_path = path_join(root_dir, "seek.log", &arena);
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  const char* old_path_value = getenv("PATH");
  char* old_path = old_path_value == NULL ? NULL : text_strdup(old_path_value);
  const char* old_tmp_value = getenv("TMPDIR");
  char* old_tmp = old_tmp_value == NULL ? NULL : text_strdup(old_tmp_value);
  const char* old_frame_value = getenv("FRAM_TEST_FRAME");
  char* old_frame = old_frame_value == NULL ? NULL : text_strdup(old_frame_value);
  const char* old_log_value = getenv("FRAM_TEST_LOG");
  char* old_log = old_log_value == NULL ? NULL : text_strdup(old_log_value);
  struct GalleryConfig config;
  init_config(&config);
  struct MediaDerivative derivatives[3] = {0};
  struct MediaItem item = {.source_path = "short.mp4",
                           .kind = MEDIA_KIND_VIDEO,
                           .derivatives = derivatives,
                           .derivative_count = 3};
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  unsigned char* logged = NULL;
  size_t logged_len = 0;
  if (write_fake_video_tools(root_dir, &arena) != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(test_jpeg_encode(2, 3, &jpeg, &jpeg_len) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(fs_write_file(frame_fixture, jpeg, jpeg_len, NULL, 0) == 0);

  TEST_CHECK(setenv("PATH", root_dir, 1) == 0);
  TEST_CHECK(setenv("TMPDIR", root_dir, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_FRAME", frame_fixture, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_LOG", log_path, 1) == 0);

  TEST_CHECK(album_scanner_probe_media(&config, media, 1, frames, 1, false, &error_buffer) == 0);
  TEST_CHECK(item.duration_ms == 400);
  TEST_CHECK(item.orientation == IMAGE_ORIENTATION_TOP_LEFT);
  TEST_CHECK(item.width_px == 2);
  TEST_CHECK(item.height_px == 3);
  TEST_CHECK(item.derivatives[0].width_px == 2);
  TEST_CHECK(item.derivatives[0].height_px == 2);
  TEST_CHECK(item.derivatives[1].width_px == 2);
  TEST_CHECK(item.derivatives[1].height_px == 1);
  TEST_CHECK(item.derivatives[2].width_px == 2);
  TEST_CHECK(item.derivatives[2].height_px == 3);
  if (!TEST_CHECK(frames[0] != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(access(frames[0], F_OK) == 0);

  TEST_CHECK(fs_read_file(log_path, TEST_FILE_LEN_MAX, &logged, &logged_len, NULL, 0) == 0);
  TEST_CHECK(strcmp((const char*)logged, "0.200") == 0);
  album_scanner_free_video_frames(frames, 1);
  TEST_CHECK(frames[0] == NULL);

cleanup:
  album_scanner_free_video_frames(frames, 1);
  free(logged);
  free(jpeg);
  string_buffer_free(&error_buffer);
  gallery_config_free(&config);
  restore_env("PATH", old_path);
  restore_env("TMPDIR", old_tmp);
  restore_env("FRAM_TEST_FRAME", old_frame);
  restore_env("FRAM_TEST_LOG", old_log);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Probe failures are associated with their source and leave image frame slots empty.
static void test_probe_media_collects_failure(void) {
  char root_dir_template[] = "/tmp/fram-scanner-failure.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena arena;
  arena_init(&arena);
  char* path = path_join(root_dir, "bad.jpg", &arena);
  TEST_CHECK(fs_write_file(path, "not jpeg", 8, NULL, 0) == 0);
  struct GalleryConfig config;
  init_config(&config);
  struct MediaItem item = {.source_path = path, .kind = MEDIA_KIND_IMAGE};
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(album_scanner_probe_media(&config, media, 1, frames, 1, false, &error_buffer) == -1);
  TEST_CHECK(strstr(error_buffer.data, "failed to probe media: failed to probe JPEG") != NULL);
  TEST_CHECK(strstr(error_buffer.data, path) != NULL);
  TEST_CHECK(frames[0] == NULL);

  string_buffer_free(&error_buffer);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A source larger than the decoder's `IMAGE_INPUT_LEN_MAX` input limit fails its probe at the read,
// from its size, naming the limit and the size, rather than loaded whole and turned away by the
// decoder. The file is sparse, so the fixture costs no disk.
static void test_probe_media_rejects_oversize_source(void) {
  char root_dir_template[] = "/tmp/fram-scanner-oversize.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena arena;
  arena_init(&arena);
  char* path = path_join(root_dir, "huge.jpg", &arena);
  const off_t source_len = (off_t)IMAGE_INPUT_LEN_MAX + 1;
  struct GalleryConfig config;
  init_config(&config);
  struct MediaItem item = {.source_path = path, .kind = MEDIA_KIND_IMAGE};
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = -1;
  TEST_CHECK(fs_write_file(path, "", 0, NULL, 0) == 0);
  if (!TEST_CHECK(truncate(path, source_len) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(album_scanner_probe_media(&config, media, 1, frames, 1, false, &error_buffer) == -1);
  expected_len =
      snprintf(expected, sizeof(expected),
               "failed to probe media: exceeds max file size (%d bytes) at %jd bytes ('%s')",
               IMAGE_INPUT_LEN_MAX, (intmax_t)source_len, path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("errors: %s", error_buffer.data);

cleanup:
  string_buffer_free(&error_buffer);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
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
    {"build skeleton rejects unsorted sources", test_build_skeleton_rejects_unsorted_sources},
    {"build skeleton rejects source outside input dir",
     test_build_skeleton_rejects_source_outside_input_dir},
    {"build skeleton rejects overlong derivative name",
     test_build_skeleton_rejects_overlong_derivative_name},
    {"build skeleton rejects overlong album path", test_build_skeleton_rejects_overlong_album_path},
    {"probe media orients image dimensions", test_probe_media_orients_image_dimensions},
    {"probe media sizes cropped derivative exactly",
     test_probe_media_sizes_cropped_derivative_exactly},
    {"probe media retains clamped video frame", test_probe_media_retains_clamped_video_frame},
    {"probe media collects failure", test_probe_media_collects_failure},
    {"probe media rejects oversize source", test_probe_media_rejects_oversize_source},
    {NULL, NULL},
};
