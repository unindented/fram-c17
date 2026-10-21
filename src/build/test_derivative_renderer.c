#include <acutest.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "build/derivative_renderer.h"
#include "build/job.h"
#include "build/test_derivative_renderer.h"
#include "build/test_job_arenas.h"
#include "build/test_jpeg.h"
#include "core/error.h"
#include "domain/gallery_config.h"
#include "domain/media_item.h"
#include "formats/image.h"
#include "os_memory.h"
#include "runtime/os.h"
#include "runtime/video.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// Every test here runs against the in-memory host: an image job reads its source and writes its
// outputs only through `runtime/os.h`. The video case, whose frame is a file `ffmpeg` writes on the
// real host, is in `test_derivative_renderer_video.c`.

/** Largest file a test here reads back, in bytes. Fixture JPEGs are a few kilobytes. */
enum { TEST_FILE_LEN_MAX = 1024 * 1024 };

/**
 * @brief Encodes a red/blue JPEG carrying EXIF orientation 6.
 *
 * The stored image is 16x8 with a red left half and a blue right half, so orientation 6 presents it
 * as a red top half over a blue bottom half.
 *
 * @param arena    Arena that receives the encoded JPEG. Must not be `NULL`.
 * @param jpeg_out Receives the encoded JPEG, which lives as long as `arena`. Must not be `NULL`.
 * @return `0` on success, or `-1` on an encode failure.
 */
static int make_oriented_jpeg(struct Arena* arena, struct Str* jpeg_out) {
  enum { WIDTH = 16, HEIGHT = 8 };
  unsigned char pixels[WIDTH * HEIGHT * IMAGE_CHANNEL_COUNT];
  for (ptrdiff_t y = 0; y < HEIGHT; y++) {
    for (ptrdiff_t x = 0; x < WIDTH; x++) {
      const ptrdiff_t offset = (y * WIDTH + x) * IMAGE_CHANNEL_COUNT;
      pixels[offset] = x < WIDTH / 2 ? 240 : 10;
      pixels[offset + 1] = 10;
      pixels[offset + 2] = x < WIDTH / 2 ? 10 : 240;
    }
  }
  const struct Image image = {.pixels = pixels, .width_px = WIDTH, .height_px = HEIGHT};
  struct Str encoded = {0};
  struct Log log = test_log();
  if (image_encode_jpeg(&image, 100, arena, &encoded, S("failed to encode fixture"),
                        S("fixture.jpg"), &log) != 0) {
    return -1;
  }
  if (encoded.len < 2) {
    return -1;
  }
  static const unsigned char app1[] = {
      0xFF, 0xE1, 0x00, 0x22, 'E',  'x',  'i',  'f',  0x00, 0x00, 'M',  'M',
      0x00, 0x2A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x01, 0x12, 0x00, 0x03,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  struct Buf oriented = {.arena = arena};
  buf_append(&oriented, str_head(encoded, 2));
  buf_append(&oriented, str_from_bytes(app1, (ptrdiff_t)sizeof(app1)));
  buf_append(&oriented, str_tail(encoded, 2));
  *jpeg_out = buf_str(&oriented);
  return 0;
}

/**
 * @brief Populates one media item with three derivatives below `public/` and an original.
 *
 * The derivatives are named `s`, `m`, and `l` and match the ones
 * `test_derivative_renderer_init_config` configures.
 *
 * @param item  Item to populate. Must not be `NULL`.
 * @param kind  Media kind of the item.
 * @param name  Source filename below `photos/`, also used as the title. Must not be `NULL`.
 * @param arena Arena that owns the item's derivatives and paths. Must not be `NULL`.
 */
static void init_item(struct MediaItem* item,
                      enum MediaKind kind,
                      const char* name,
                      struct Arena* arena) {
  struct MediaDerivative* derivatives = arena_new(arena, 3, struct MediaDerivative);
  derivatives[0] = (struct MediaDerivative){
      .name = S("s"), .path = S("public/s.jpg"), .width_px = 2, .height_px = 2};
  derivatives[1] = (struct MediaDerivative){
      .name = S("m"), .path = S("public/m.jpg"), .width_px = 8, .height_px = 4};
  derivatives[2] = (struct MediaDerivative){
      .name = S("l"), .path = S("public/l.jpg"), .width_px = 8, .height_px = 4};
  *item = (struct MediaItem){
      .source_path = str_concat(S("photos/"), str_from_cstr(name), arena),
      .title = str_clone(str_from_cstr(name), arena),
      .kind = kind,
      .derivatives = derivatives,
      .derivative_count = 3,
      .original_path = S("public/original.bin"),
      .orientation = IMAGE_ORIENTATION_TOP_LEFT,
      .width_px = 8,
      .height_px = 4,
  };
}

/**
 * @brief Points a media item's outputs at their own files below an output directory.
 *
 * `init_item` gives every item the same output paths, which suits a single item. A test with
 * several items calls this after `init_item` so the jobs do not write one another's files.
 *
 * @param item       Item `init_item` populated. Must not be `NULL`.
 * @param output_dir Terminated output directory. Must not be `NULL`.
 * @param stem       Terminated output filename stem, distinct per item. Must not be `NULL`.
 * @param arena      Arena that owns the new paths. Must not be `NULL`.
 */
static void relocate_item_outputs(struct MediaItem* item,
                                  const char* output_dir,
                                  const char* stem,
                                  struct Arena* arena) {
  for (ptrdiff_t i = 0; i < item->derivative_count; i++) {
    struct Buf path = {.arena = arena};
    buf_printf(&path, "%s/%s-%.*s.jpg", output_dir, stem, STR_FMT(item->derivatives[i].name));
    item->derivatives[i].path = buf_str(&path);
  }
  struct Buf path = {.arena = arena};
  buf_printf(&path, "%s/originals/%s.jpg", output_dir, stem);
  item->original_path = buf_str(&path);
}

/**
 * @brief Reads one generated JPEG and probes its dimensions, recording why when it cannot.
 *
 * @param path       JPEG file to read.
 * @param os         Host holding the file. Must not be `NULL`.
 * @param scratch    Arena for the file bytes.
 * @param width_out  Receives the width in pixels on success. Must not be `NULL`.
 * @param height_out Receives the height in pixels on success. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file cannot be read or probed.
 */
static int probe_file(struct Str path,
                      struct Os* os,
                      struct Arena scratch,
                      ptrdiff_t* width_out,
                      ptrdiff_t* height_out) {
  struct Str data = {0};
  struct Log log = test_log();
  const int rc =
      os_read(os, path, TEST_FILE_LEN_MAX, &scratch, &data, S("failed to read output"), &log) == 0
          ? image_probe(data, width_out, height_out, S("failed to probe output"), path, &log)
          : -1;
  const struct Str failure = buf_str(&log.buf);
  TEST_MSG("%.*s", STR_FMT(failure));
  return rc;
}

/**
 * @brief Reports whether two `os_stat` results carry the same modification time.
 *
 * @param a First result. Must not be `NULL`.
 * @param b Second result. Must not be `NULL`.
 * @return `true` when both the seconds and the nanoseconds agree.
 */
static bool is_same_mtime(const struct OsStat* a, const struct OsStat* b) {
  return a->mtime_s == b->mtime_s && a->mtime_ns == b->mtime_ns;
}

/**
 * @brief Runs a derivative phase over fresh worker arenas, reading sources up to the decoder's own
 *        input limit.
 *
 * @param gallery_config Configured derivatives shared by every job. Must not be `NULL`.
 * @param media          Media items, one job per item. Must not be `NULL`.
 * @param media_count    Number of items in `media`.
 * @param video_frames   Video frame slots, one per item in `media`. Must not be `NULL`.
 * @param worker_count   Number of worker threads to request.
 * @param is_verbose     Whether each finished job prints a progress line.
 * @param os             Host holding the sources, which receives the outputs. Must not be `NULL`.
 * @param log            Log that receives derivative errors. Must not be `NULL`.
 * @return The renderer result.
 */
static int generate(const struct GalleryConfig* gallery_config,
                    struct MediaItem* const* media,
                    ptrdiff_t media_count,
                    struct VideoFrame* video_frames,
                    ptrdiff_t worker_count,
                    bool is_verbose,
                    struct Os* os,
                    struct Log* log) {
  struct Arena arenas_perm = test_arena();
  return derivative_renderer_generate(gallery_config, media, media_count, video_frames,
                                      worker_count, test_job_arenas(worker_count, &arenas_perm),
                                      is_verbose, os, test_arena(), log);
}

/**
 * @brief Generates the derivatives of the orientation-6 JPEG from `make_oriented_jpeg`, padded with
 *        `NUL` bytes to `source_len`, and checks the large one and the copied original's size.
 *
 * Orientation 6 turns a stored left/right color split into a presented top/bottom split. Checking
 * both regions distinguishes it from a distorted resize.
 *
 * @param source_len Size of the source file in bytes. A value below the JPEG's own length leaves it
 *                   unpadded.
 */
static void check_generate_applies_orientation(ptrdiff_t source_len) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct MediaItem item;
  struct Str jpeg = {0};
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 4, 16, 16, 95);
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  struct Image decoded = {0};
  init_item(&item, MEDIA_KIND_IMAGE, "oriented.jpg", &arena);
  item.derivatives[0].width_px = 2;
  item.derivatives[0].height_px = 4;
  item.derivatives[2].width_px = 8;
  item.derivatives[2].height_px = 16;
  if (!TEST_CHECK(make_oriented_jpeg(&arena, &jpeg) == 0)) {
    return;
  }
  os_memory_write_bytes(os, item.source_path, jpeg);
  const char* source = str_cstr(item.source_path, &arena);
  os_memory_truncate(os, source, source_len > jpeg.len ? source_len : jpeg.len);

  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == 0);
  struct Str output = {0};
  TEST_CHECK(os_memory_file(os, "public/l.jpg", &output) == 0);
  TEST_CHECK(image_decode(output, &decoded, S("failed to decode output"), S("output.jpg"), &log) ==
             0);
  if (TEST_CHECK(decoded.width_px == 8 && decoded.height_px == 16)) {
    const ptrdiff_t top = (2 * decoded.width_px + 4) * IMAGE_CHANNEL_COUNT;
    const ptrdiff_t bottom = (13 * decoded.width_px + 4) * IMAGE_CHANNEL_COUNT;
    TEST_CHECK(decoded.pixels[top] > decoded.pixels[top + 2]);
    TEST_CHECK(decoded.pixels[bottom + 2] > decoded.pixels[bottom]);
  }
  image_free(&decoded);
  struct OsStat original = {0};
  TEST_CHECK(os_stat(os, item.original_path, &original) == 0 &&
             original.len == (source_len > jpeg.len ? source_len : jpeg.len));
}

// Stale image outputs are generated at their planned dimensions and the original is copied
// byte-for-byte. A second run preserves every output timestamp.
static void test_generate_image_and_skip_fresh_outputs(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct MediaItem item;
  struct Str jpeg = {0};
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 90);
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  ptrdiff_t width = 0;
  ptrdiff_t height = 0;
  init_item(&item, MEDIA_KIND_IMAGE, "source.jpg", &arena);
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &arena, &jpeg) == 0)) {
    return;
  }
  os_memory_write_bytes(os, item.source_path, jpeg);

  TEST_CHECK(generate(&config, media, 1, frames, 2, false, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));

  TEST_CHECK(probe_file(item.derivatives[0].path, os, arena, &width, &height) == 0);
  TEST_CHECK(width == 2 && height == 2);
  TEST_CHECK(probe_file(item.derivatives[1].path, os, arena, &width, &height) == 0);
  TEST_CHECK(width == 8 && height == 4);
  TEST_CHECK(probe_file(item.derivatives[2].path, os, arena, &width, &height) == 0);
  TEST_CHECK(width == 8 && height == 4);
  struct Str original = {0};
  TEST_CHECK(os_memory_file(os, "public/original.bin", &original) == 0 && str_eq(original, jpeg));

  struct OsStat thumb_before;
  struct OsStat large_before;
  struct OsStat original_before;
  TEST_CHECK(os_stat(os, item.derivatives[0].path, &thumb_before) == 0);
  TEST_CHECK(os_stat(os, item.derivatives[2].path, &large_before) == 0);
  TEST_CHECK(os_stat(os, item.original_path, &original_before) == 0);
  TEST_CHECK(generate(&config, media, 1, frames, 2, false, os, &log) == 0);
  struct OsStat after;
  TEST_CHECK(os_stat(os, item.derivatives[0].path, &after) == 0 &&
             is_same_mtime(&after, &thumb_before));
  TEST_CHECK(os_stat(os, item.derivatives[2].path, &after) == 0 &&
             is_same_mtime(&after, &large_before));
  TEST_CHECK(os_stat(os, item.original_path, &after) == 0 &&
             is_same_mtime(&after, &original_before));
}

// Freshness is decided separately: a fresh thumbnail remains untouched while a zero-byte large
// derivative and a same-age original of the wrong size are regenerated.
static void test_generate_refreshes_outputs_independently(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct MediaItem item;
  struct Str jpeg = {0};
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  init_item(&item, MEDIA_KIND_IMAGE, "source.jpg", &arena);
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &arena, &jpeg) == 0)) {
    return;
  }
  os_memory_write_bytes(os, item.source_path, jpeg);

  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == 0);
  struct OsStat thumb_before;
  TEST_CHECK(os_stat(os, item.derivatives[0].path, &thumb_before) == 0);
  os_memory_write(os, "public/l.jpg", "");
  os_memory_write(os, "public/original.bin", "x");
  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == 0);

  struct OsStat after;
  TEST_CHECK(os_stat(os, item.derivatives[0].path, &after) == 0 &&
             is_same_mtime(&after, &thumb_before));
  TEST_CHECK(os_stat(os, item.derivatives[2].path, &after) == 0 && after.len > 0);
  TEST_CHECK(os_stat(os, item.original_path, &after) == 0 && after.len == jpeg.len);
}

// When both image derivatives and the original are fresh, the renderer never decodes the source.
// Replacing it with same-sized invalid bytes, then making every output newer than it again,
// therefore still succeeds.
static void test_generate_avoids_decode_when_derivatives_fresh(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct MediaItem item;
  struct Str jpeg = {0};
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  init_item(&item, MEDIA_KIND_IMAGE, "source.jpg", &arena);
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &arena, &jpeg) == 0)) {
    return;
  }
  os_memory_write_bytes(os, item.source_path, jpeg);
  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == 0);

  char* invalid = arena_new(&arena, jpeg.len, char);
  os_memory_write_bytes(os, item.source_path, (struct Str){invalid, jpeg.len});
  static const char* const outputs[] = {"public/s.jpg", "public/m.jpg", "public/l.jpg",
                                        "public/original.bin"};
  for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
    os_memory_touch(os, outputs[i]);
  }
  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// Image sources apply EXIF before resizing.
static void test_generate_applies_image_exif_orientation(void) {
  check_generate_applies_orientation(0);
}

// A source large enough to give its pages back is oriented as one that keeps them, so the
// orientation is read before the source is given back.
static void test_generate_orients_large_source_before_giving_it_back(void) {
  check_generate_applies_orientation(JOB_SOURCE_DISCARD_LEN_MIN);
}

// A verbose phase prints one progress line per finished job, then closes the line. Each worker
// prints its whole line in one `os_print` call under the lock `job.c` keeps for the progress line.
// This drives it with more workers than one so the lock is contended, and the `tsan` test preset
// runs the same path under ThreadSanitizer.
static void test_verbose_prints_one_progress_line_per_job(void) {
  enum { ITEM_COUNT = 3 };
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct Str jpeg = {0};
  struct MediaItem items[ITEM_COUNT];
  struct MediaItem* media[ITEM_COUNT];
  struct VideoFrame frames[ITEM_COUNT] = {{0}};
  static const char* const names[ITEM_COUNT] = {"a.jpg", "b.jpg", "c.jpg"};
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct Log log = test_log();
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &arena, &jpeg) == 0)) {
    return;
  }
  for (ptrdiff_t i = 0; i < ITEM_COUNT; i++) {
    init_item(&items[i], MEDIA_KIND_IMAGE, names[i], &arena);
    relocate_item_outputs(&items[i], "public", names[i], &arena);
    os_memory_write_bytes(os, items[i].source_path, jpeg);
    media[i] = &items[i];
  }

  TEST_CHECK(generate(&config, media, ITEM_COUNT, frames, 4, true, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // Three jobs, so three progress lines, and one trailing newline closing the line before any later
  // status message. Workers finish in any order, so each count is asserted present once rather than
  // the whole text in sequence.
  const struct Str printed = os_memory_printed(os, 2);
  const char* text = str_cstr(printed, &arena);
  static const char* const progress_lines[] = {"\rgenerating derivatives 1/3",
                                               "\rgenerating derivatives 2/3",
                                               "\rgenerating derivatives 3/3"};
  size_t progress_len = 0;
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(progress_lines) / sizeof(progress_lines[0])); i++) {
    const char* found = strstr(text, progress_lines[i]);
    TEST_CHECK(found != NULL && strstr(found + 1, progress_lines[i]) == NULL);
    progress_len += strlen(progress_lines[i]);
  }
  TEST_CHECK(strlen(text) == progress_len + 1);
  TEST_CHECK(text[progress_len] == '\n');
  TEST_MSG("stderr: '%s'", text);
}

// Derivatives that share parent directories are written concurrently, each job creating the parents
// its own outputs need. Every job races its siblings to create `public/trip` and
// `public/trip/deep`, so a job that took a sibling's `EEXIST` for a failure would drop its
// derivatives. The `tsan` test preset runs the same path under ThreadSanitizer.
static void test_writes_derivatives_sharing_parents_concurrently(void) {
  enum { ITEM_COUNT = 16 };
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct Str jpeg = {0};
  struct MediaItem items[ITEM_COUNT];
  struct MediaItem* media[ITEM_COUNT];
  struct VideoFrame frames[ITEM_COUNT] = {{0}};
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct Log log = test_log();
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &arena, &jpeg) == 0)) {
    return;
  }
  for (ptrdiff_t i = 0; i < ITEM_COUNT; i++) {
    char name[64];
    (void)snprintf(name, sizeof(name), "trip/deep/one-%02td.jpg", i);
    char stem[32];
    (void)snprintf(stem, sizeof(stem), "one-%02td-jpg", i);
    init_item(&items[i], MEDIA_KIND_IMAGE, name, &arena);
    relocate_item_outputs(&items[i], "public/trip/deep", stem, &arena);
    os_memory_write_bytes(os, items[i].source_path, jpeg);
    media[i] = &items[i];
  }

  TEST_CHECK(generate(&config, media, ITEM_COUNT, frames, 8, false, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));

  for (ptrdiff_t i = 0; i < ITEM_COUNT; i++) {
    ptrdiff_t width = 0;
    ptrdiff_t height = 0;
    TEST_CHECK(probe_file(items[i].derivatives[0].path, os, arena, &width, &height) == 0);
    TEST_CHECK(width == 2 && height == 2);
    TEST_CHECK(probe_file(items[i].derivatives[1].path, os, arena, &width, &height) == 0);
    TEST_CHECK(width == 8 && height == 4);
    TEST_CHECK(probe_file(items[i].derivatives[2].path, os, arena, &width, &height) == 0);
    TEST_CHECK(width == 8 && height == 4);
    struct OsStat original;
    TEST_CHECK(os_stat(os, items[i].original_path, &original) == 0);
    TEST_MSG("item %td", i);
  }
}

// The phase accepts an empty media set, for which `job_run` allocates no error slot and starts no
// job.
static void test_accepts_empty_media_set(void) {
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[1] = {NULL};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();

  TEST_CHECK(generate(&config, media, 0, frames, 1, false, os_memory(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// A derivative whose path is a symlink is written through it, as every output is: the file it names
// receives the JPEG.
static void test_derivative_write_follows_symlink(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct MediaItem item;
  struct Str jpeg = {0};
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  init_item(&item, MEDIA_KIND_IMAGE, "source.jpg", &arena);
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &arena, &jpeg) == 0)) {
    return;
  }
  os_memory_write_bytes(os, item.source_path, jpeg);
  // An empty target is stale, so the build regenerates the derivative behind the link.
  os_memory_write(os, "elsewhere/l.jpg", "");
  os_memory_mkdir(os, "public");
  os_memory_symlink(os, "../elsewhere/l.jpg", "public/l.jpg");

  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  ptrdiff_t width = 0;
  ptrdiff_t height = 0;
  TEST_CHECK(probe_file(S("elsewhere/l.jpg"), os, arena, &width, &height) == 0);
  TEST_CHECK(width == 8 && height == 4);
}

// A derivative whose write fails is reported. The next build regenerates it once the cause is gone,
// because the failed write left no file: the in-memory host fails it before the first byte. A write
// cut short part-way would leave a file the next build keeps as fresh, as `is_fresh_derivative`
// documents. The outputs that did land are left alone.
static void test_failed_derivative_write_is_regenerated(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct MediaItem item;
  struct Str jpeg = {0};
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  init_item(&item, MEDIA_KIND_IMAGE, "source.jpg", &arena);
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &arena, &jpeg) == 0)) {
    return;
  }
  os_memory_write_bytes(os, item.source_path, jpeg);
  os_memory_fail(os, OS_MEMORY_OP_WRITE_DATA, "public/l.jpg", ENOSPC);

  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == -1);
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  TEST_CHECK(test_log_is(&log, "failed to write derivative 'l': %s ('public/l.jpg')\n",
                         error_system_message(reason, sizeof(reason), ENOSPC)));
  struct OsStat thumb_before;
  TEST_CHECK(os_stat(os, item.derivatives[0].path, &thumb_before) == 0);

  os_memory_fail(os, OS_MEMORY_OP_WRITE_DATA, "public/l.jpg", 0);
  log = test_log();
  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  ptrdiff_t width = 0;
  ptrdiff_t height = 0;
  TEST_CHECK(probe_file(item.derivatives[2].path, os, arena, &width, &height) == 0);
  TEST_CHECK(width == 8 && height == 4);
  struct OsStat after;
  TEST_CHECK(os_stat(os, item.derivatives[0].path, &after) == 0 &&
             is_same_mtime(&after, &thumb_before));
}

// A read failure on a long source path is reported in full: the cause, the limit and the measured
// length, and the whole path. The source and the fresh original are sparse, so the fixture costs no
// memory.
static void test_reports_read_failure_with_long_source_path(void) {
  // Three 200-byte directories make the source path alone longer than 600 bytes.
  enum { SEGMENT_LEN = 200, DEPTH = 3 };
  char name[DEPTH * (SEGMENT_LEN + 1) + sizeof("source.jpg")];
  ptrdiff_t name_len = 0;
  for (ptrdiff_t i = 0; i < DEPTH; i++) {
    memset(name + name_len, 'a', SEGMENT_LEN);
    name_len += SEGMENT_LEN;
    name[name_len++] = '/';
  }
  memcpy(name + name_len, "source.jpg", sizeof("source.jpg"));

  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct MediaItem item;
  const ptrdiff_t source_len = (ptrdiff_t)IMAGE_INPUT_LEN_MAX + 1;
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  init_item(&item, MEDIA_KIND_IMAGE, name, &arena);
  const char* source = str_cstr(item.source_path, &arena);
  os_memory_write(os, source, "");
  os_memory_truncate(os, source, source_len);
  // An original of the same size written after the source is fresh, so the read failure is the
  // job's only diagnostic.
  os_memory_write(os, "public/original.bin", "");
  os_memory_truncate(os, "public/original.bin", source_len);

  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to read derivative source: exceeds max file size (%d bytes) at "
                         "%td bytes ('%.*s')\n",
                         IMAGE_INPUT_LEN_MAX, source_len, STR_FMT(item.source_path)));
}

// A source that exists but cannot be read is reported with the system cause and the path it
// happened on, rather than as a decode failure. The derivatives are stale, so the job reaches the
// read, and the original is fresh, so the read failure is its only diagnostic.
static void test_reports_unreadable_source(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct MediaItem item;
  struct Str jpeg = {0};
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  init_item(&item, MEDIA_KIND_IMAGE, "source.jpg", &arena);
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &arena, &jpeg) == 0)) {
    return;
  }
  os_memory_write_bytes(os, item.source_path, jpeg);
  os_memory_write_bytes(os, item.original_path, jpeg);
  os_memory_fail(os, OS_MEMORY_OP_READ, "photos/source.jpg", EACCES);

  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  TEST_CHECK(test_log_is(&log, "failed to read derivative source: %s ('photos/source.jpg')\n",
                         error_system_message(reason, sizeof(reason), EACCES)));
}

// A source that is a directory by the time its job runs is reported once, as a source that is not a
// regular file, rather than once by the decode and again by the copy of the original.
static void test_reports_directory_source_once(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct MediaItem item;
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  init_item(&item, MEDIA_KIND_IMAGE, "source.jpg", &arena);
  os_memory_mkdir(os, "photos/source.jpg");

  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to inspect media source: not a regular file ('photos/source.jpg')\n"));
  TEST_CHECK(os_memory_files(os, "public").count == 0);
}

// A derivative source larger than the decoder's `IMAGE_INPUT_LEN_MAX` input limit is rejected at
// the read, from its size, naming the limit and the size, rather than loaded whole and turned away
// by the decoder. The source and the fresh original are sparse, so the fixture costs no memory.
static void test_rejects_oversize_source(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct MediaItem item;
  const ptrdiff_t source_len = (ptrdiff_t)IMAGE_INPUT_LEN_MAX + 1;
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct Log log = test_log();
  init_item(&item, MEDIA_KIND_IMAGE, "source.jpg", &arena);
  os_memory_write(os, "photos/source.jpg", "");
  os_memory_truncate(os, "photos/source.jpg", source_len);
  // An original of the same size written after the source is fresh, so the job reaches the
  // derivative read without first copying two gigabytes.
  os_memory_write(os, "public/original.bin", "");
  os_memory_truncate(os, "public/original.bin", source_len);

  TEST_CHECK(generate(&config, media, 1, frames, 1, false, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to read derivative source: exceeds max file size (%d bytes) at "
                         "%td bytes ('photos/source.jpg')\n",
                         IMAGE_INPUT_LEN_MAX, source_len));
}

// Two failing sources append one line each to the log, in media order, each ending in exactly one
// newline. That is what `job_run`'s merge of the jobs' logs means at this module's boundary, and a
// single-source failure cannot observe the order at all.
// `test_reports_one_line_per_failing_media_item` in `src/app/test_cmd_build.c` pins the same
// separator for failures in the probe phase, which stops a build before this phase runs. This one
// pins it at the module boundary, where the contract is documented.
static void test_appends_one_error_line_per_failing_source(void) {
  // Both sources are not JPEG data, so each fails its decode with its own message.
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct MediaItem items[2];
  static const char* const names[] = {"a.jpg", "b.jpg"};
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&items[0], &items[1]};
  struct VideoFrame frames[2] = {{0}};
  struct Log log = test_log();
  for (ptrdiff_t i = 0; i < 2; i++) {
    init_item(&items[i], MEDIA_KIND_IMAGE, names[i], &arena);
    relocate_item_outputs(&items[i], "public", names[i], &arena);
    os_memory_write_bytes(os, items[i].source_path, S("not a JPEG\n"));
  }

  TEST_CHECK(generate(&config, media, 2, frames, 2, false, os, &log) == -1);
  // Compared whole: a dropped newline, a lost line, or the wrong order each change these bytes.
  TEST_CHECK(test_log_is(&log,
                         "failed to decode derivative source: failed to probe JPEG: unknown image "
                         "type ('photos/a.jpg')\n"
                         "failed to decode derivative source: failed to probe JPEG: unknown image "
                         "type ('photos/b.jpg')\n"));
  TEST_CHECK(log.errors == 2);
}

TEST_LIST = {
    {"generate image and skip fresh outputs", test_generate_image_and_skip_fresh_outputs},
    {"generate refreshes outputs independently", test_generate_refreshes_outputs_independently},
    {"generate avoids decode when derivatives fresh",
     test_generate_avoids_decode_when_derivatives_fresh},
    {"generate applies image exif orientation", test_generate_applies_image_exif_orientation},
    {"generate orients large source before giving it back",
     test_generate_orients_large_source_before_giving_it_back},
    {"verbose prints one progress line per job", test_verbose_prints_one_progress_line_per_job},
    {"writes derivatives sharing parents concurrently",
     test_writes_derivatives_sharing_parents_concurrently},
    {"accepts empty media set", test_accepts_empty_media_set},
    {"derivative write follows symlink", test_derivative_write_follows_symlink},
    {"failed derivative write is regenerated", test_failed_derivative_write_is_regenerated},
    {"reports read failure with long source path", test_reports_read_failure_with_long_source_path},
    {"reports unreadable source", test_reports_unreadable_source},
    {"reports directory source once", test_reports_directory_source_once},
    {"rejects oversize source", test_rejects_oversize_source},
    {"appends one error line per failing source", test_appends_one_error_line_per_failing_source},
    {NULL, NULL},
};
