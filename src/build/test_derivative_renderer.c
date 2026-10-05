#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "build/derivative_renderer.h"
#include "build/test_jpeg.h"
#include "core/error.h"
#include "core/path.h"
#include "domain/gallery_config.h"
#include "domain/media_item.h"
#include "formats/image.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Fixture JPEGs are a few kilobytes. */
enum { TEST_FILE_LEN_MAX = 1024 * 1024 };

/**
 * @brief Encodes a red/blue JPEG carrying EXIF orientation 6.
 *
 * The stored image is 16x8 with a red left half and a blue right half, so orientation 6 presents it
 * as a red top half over a blue bottom half.
 *
 * @param data_out     Receives the encoded JPEG the caller must `free`. Must not be `NULL`.
 * @param data_len_out Receives the length of the encoded JPEG in bytes. Must not be `NULL`.
 * @return `0` on success, or `-1` on an encode or allocation failure.
 */
static int make_oriented_jpeg(unsigned char** data_out, size_t* data_len_out) {
  enum { WIDTH = 16, HEIGHT = 8 };
  unsigned char pixels[WIDTH * HEIGHT * IMAGE_CHANNEL_COUNT];
  for (size_t y = 0; y < HEIGHT; y++) {
    for (size_t x = 0; x < WIDTH; x++) {
      const size_t offset = (y * WIDTH + x) * IMAGE_CHANNEL_COUNT;
      pixels[offset] = x < WIDTH / 2 ? 240 : 10;
      pixels[offset + 1] = 10;
      pixels[offset + 2] = x < WIDTH / 2 ? 10 : 240;
    }
  }
  const struct Image image = {.pixels = pixels, .width_px = WIDTH, .height_px = HEIGHT};
  unsigned char* encoded = NULL;
  size_t encoded_len = 0;
  if (image_encode_jpeg(&image, 100, &encoded, &encoded_len, NULL, 0) != 0 || encoded_len < 2) {
    free(encoded);
    return -1;
  }
  static const unsigned char app1[] = {
      0xFF, 0xE1, 0x00, 0x22, 'E',  'x',  'i',  'f',  0x00, 0x00, 'M',  'M',
      0x00, 0x2A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x01, 0x12, 0x00, 0x03,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  if (encoded_len > SIZE_MAX - sizeof(app1)) {
    free(encoded);
    return -1;
  }
  unsigned char* oriented = malloc(encoded_len + sizeof(app1));
  if (oriented == NULL) {
    free(encoded);
    return -1;
  }
  memcpy(oriented, encoded, 2);
  memcpy(oriented + 2, app1, sizeof(app1));
  memcpy(oriented + 2 + sizeof(app1), encoded + 2, encoded_len - 2);
  free(encoded);
  *data_out = oriented;
  *data_len_out = encoded_len + sizeof(app1);
  return 0;
}

/**
 * @brief Populates one media item with three derivatives and an original below `root_dir`.
 *
 * The derivatives are named `s`, `m`, and `l` and match the ones `init_config` configures.
 *
 * @param item     Item to populate. Must not be `NULL`.
 * @param kind     Media kind of the item.
 * @param root_dir Fixture root the source and output paths are joined under. Must not be `NULL`.
 * @param name     Source filename relative to `root_dir`, also used as the title. Must not be
 *                 `NULL`.
 * @param arena    Arena that owns the item's derivatives and paths. Must not be `NULL`.
 * @return `0` on success, or `-1` after failing the current test check.
 */
static int init_item(struct MediaItem* item,
                     enum MediaKind kind,
                     const char* root_dir,
                     const char* name,
                     struct Arena* arena) {
  struct MediaDerivative* derivatives = arena_calloc(arena, 3, sizeof(*derivatives));
  if (!TEST_CHECK(derivatives != NULL)) {
    return -1;
  }
  derivatives[0] = (struct MediaDerivative){
      .name = "s",
      .path = path_join(root_dir, "public/s.jpg", arena),
      .width_px = 2,
      .height_px = 2,
  };
  derivatives[1] = (struct MediaDerivative){
      .name = "m",
      .path = path_join(root_dir, "public/m.jpg", arena),
      .width_px = 8,
      .height_px = 4,
  };
  derivatives[2] = (struct MediaDerivative){
      .name = "l",
      .path = path_join(root_dir, "public/l.jpg", arena),
      .width_px = 8,
      .height_px = 4,
  };
  *item = (struct MediaItem){
      .source_path = path_join(root_dir, name, arena),
      .title = name,
      .kind = kind,
      .derivatives = derivatives,
      .derivative_count = 3,
      .original_path = path_join(root_dir, "public/original.bin", arena),
      .orientation = IMAGE_ORIENTATION_TOP_LEFT,
      .width_px = 8,
      .height_px = 4,
  };
  return 0;
}

/**
 * @brief Points a media item's outputs at their own files below an output directory.
 *
 * `init_item` gives every item the same output paths, which suits a single item. A test with
 * several items calls this after `init_item` so the jobs do not write one another's files.
 *
 * @param item       Item `init_item` populated. Must not be `NULL`.
 * @param root_dir   Fixture root the output paths are joined under. Must not be `NULL`.
 * @param output_dir Output directory relative to `root_dir`. Must not be `NULL`.
 * @param stem       Output filename stem, distinct per item. Must not be `NULL`.
 * @param arena      Arena that owns the new paths. Must not be `NULL`.
 * @return `0` on success, or `-1` after failing the current test check.
 */
static int relocate_item_outputs(struct MediaItem* item,
                                 const char* root_dir,
                                 const char* output_dir,
                                 const char* stem,
                                 struct Arena* arena) {
  char relative[256];
  for (size_t i = 0; i < item->derivative_count; i++) {
    const int relative_len = snprintf(relative, sizeof(relative), "%s/%s-%s.jpg", output_dir, stem,
                                      item->derivatives[i].name);
    if (!TEST_CHECK(relative_len > 0 && (size_t)relative_len < sizeof(relative))) {
      return -1;
    }
    item->derivatives[i].path = path_join(root_dir, relative, arena);
    if (!TEST_CHECK(item->derivatives[i].path != NULL)) {
      return -1;
    }
  }
  const int relative_len =
      snprintf(relative, sizeof(relative), "%s/originals/%s.jpg", output_dir, stem);
  if (!TEST_CHECK(relative_len > 0 && (size_t)relative_len < sizeof(relative))) {
    return -1;
  }
  item->original_path = path_join(root_dir, relative, arena);
  return TEST_CHECK(item->original_path != NULL) ? 0 : -1;
}

/**
 * @brief Initializes a configuration with the three derivatives `init_item` plans.
 *
 * `s` and `m` crop, and `l` fits. The derivative array is static, so the configuration needs no
 * release and only one may be live at a time.
 *
 * @param config       Configuration to initialize. Must not be `NULL`.
 * @param small_width  Width of the `s` derivative in pixels.
 * @param small_height Height of the `s` derivative in pixels.
 * @param large_width  Width bound of the `l` derivative in pixels.
 * @param large_height Height bound of the `l` derivative in pixels.
 * @param quality      JPEG quality shared by every derivative, from 1 through 100.
 */
static void init_config(struct GalleryConfig* config,
                        size_t small_width,
                        size_t small_height,
                        size_t large_width,
                        size_t large_height,
                        size_t quality) {
  static struct GalleryDerivative derivatives[3];
  derivatives[0] = (struct GalleryDerivative){.name = "s",
                                              .width_px = small_width,
                                              .height_px = small_height,
                                              .quality = quality,
                                              .is_crop = true};
  derivatives[1] = (struct GalleryDerivative){
      .name = "m", .width_px = 8, .height_px = 4, .quality = quality, .is_crop = true};
  derivatives[2] = (struct GalleryDerivative){.name = "l",
                                              .width_px = large_width,
                                              .height_px = large_height,
                                              .quality = quality,
                                              .is_crop = false};
  *config = (struct GalleryConfig){
      .derivatives = derivatives,
      .derivative_count = 3,
  };
}

/**
 * @brief Reads one generated JPEG and probes its dimensions.
 *
 * @param path       JPEG file to read. Must not be `NULL`.
 * @param width_out  Receives the width in pixels on success. Must not be `NULL`.
 * @param height_out Receives the height in pixels on success. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file cannot be read or probed.
 */
static int probe_file(const char* path, size_t* width_out, size_t* height_out) {
  unsigned char* data = NULL;
  size_t data_len = 0;
  if (fs_read_file(path, TEST_FILE_LEN_MAX, &data, &data_len, NULL, 0) != 0) {
    return -1;
  }
  const int rc = image_probe(data, data_len, width_out, height_out, NULL, 0);
  free(data);
  return rc;
}

/**
 * @brief Backdates a file's access and modification times to before a reference timestamp.
 *
 * @param path      File to backdate. Must not be `NULL`.
 * @param reference Metadata whose modification time the file must end up older than. Must not be
 *                  `NULL`.
 * @return `0` on success, or `-1` with `errno` set when the times cannot be changed.
 */
static int set_older_mtime(const char* path, const struct FsMeta* reference) {
  const time_t seconds = (time_t)(reference->mtime_s > 10 ? reference->mtime_s - 10 : 0);
  const struct timespec times[] = {
      {.tv_sec = seconds, .tv_nsec = reference->mtime_ns},
      {.tv_sec = seconds, .tv_nsec = reference->mtime_ns},
  };
  return utimensat(AT_FDCWD, path, times, 0);
}

/**
 * @brief Runs a verbose derivative phase while capturing standard error.
 *
 * Restores standard error before returning.
 *
 * @param gallery_config    Configured derivatives shared by every job.
 * @param media             Media items, one job per item.
 * @param media_count       Number of items in `media`.
 * @param video_frame_paths Video frame paths, one per item in `media`.
 * @param worker_count      Number of worker threads to request.
 * @param error_out         Buffer that receives derivative diagnostics.
 * @param stderr_out        Buffer that receives terminated standard error.
 * @param stderr_out_len    Size of `stderr_out` in bytes. Must be non-zero.
 * @return The renderer result, or `TEST_PLUMBING_FAILED` on test-plumbing failure.
 */
static int generate_capturing_stderr(const struct GalleryConfig* gallery_config,
                                     struct MediaItem* const* media,
                                     size_t media_count,
                                     char* const* video_frame_paths,
                                     size_t worker_count,
                                     struct StringBuffer* error_out,
                                     char* stderr_out,
                                     size_t stderr_out_len) {
  stderr_out[0] = '\0';
  struct StreamCapture stderr_capture;
  if (capture_begin(stderr, &stderr_capture) != 0) {
    return TEST_PLUMBING_FAILED;
  }
  const int rc = derivative_renderer_generate(gallery_config, media, media_count, video_frame_paths,
                                              worker_count, true, error_out);
  return capture_end(&stderr_capture, stderr_out, stderr_out_len) == 0 ? rc : TEST_PLUMBING_FAILED;
}

// Stale image outputs are generated at their planned dimensions and the original is copied
// byte-for-byte. A second run preserves every output timestamp.
static void test_generate_image_and_skip_fresh_outputs(void) {
  char root_dir_template[] = "/tmp/fram-derivative-image.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  struct MediaItem item;
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  struct GalleryConfig config;
  init_config(&config, 2, 2, 8, 8, 90);
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  size_t width = 0;
  size_t height = 0;
  unsigned char* original = NULL;
  size_t original_len = 0;
  if (init_item(&item, MEDIA_KIND_IMAGE, root_dir, "source.jpg", &arena) != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &jpeg, &jpeg_len) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.source_path, jpeg, jpeg_len, NULL, 0) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(derivative_renderer_generate(&config, media, 1, frames, 2, false, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);

  TEST_CHECK(probe_file(item.derivatives[0].path, &width, &height) == 0);
  TEST_CHECK(width == 2 && height == 2);
  TEST_CHECK(probe_file(item.derivatives[1].path, &width, &height) == 0);
  TEST_CHECK(width == 8 && height == 4);
  TEST_CHECK(probe_file(item.derivatives[2].path, &width, &height) == 0);
  TEST_CHECK(width == 8 && height == 4);

  TEST_CHECK(
      fs_read_file(item.original_path, TEST_FILE_LEN_MAX, &original, &original_len, NULL, 0) == 0);
  TEST_CHECK(original_len == jpeg_len && memcmp(original, jpeg, jpeg_len) == 0);

  struct FsMeta thumb_before;
  struct FsMeta large_before;
  struct FsMeta original_before;
  TEST_CHECK(fs_stat_meta(item.derivatives[0].path, &thumb_before, NULL, 0) == 0);
  TEST_CHECK(fs_stat_meta(item.derivatives[2].path, &large_before, NULL, 0) == 0);
  TEST_CHECK(fs_stat_meta(item.original_path, &original_before, NULL, 0) == 0);
  TEST_CHECK(derivative_renderer_generate(&config, media, 1, frames, 2, false, &error_buffer) == 0);
  struct FsMeta after;
  TEST_CHECK(fs_stat_meta(item.derivatives[0].path, &after, NULL, 0) == 0);
  TEST_CHECK(after.mtime_s == thumb_before.mtime_s && after.mtime_ns == thumb_before.mtime_ns);
  TEST_CHECK(fs_stat_meta(item.derivatives[2].path, &after, NULL, 0) == 0);
  TEST_CHECK(after.mtime_s == large_before.mtime_s && after.mtime_ns == large_before.mtime_ns);
  TEST_CHECK(fs_stat_meta(item.original_path, &after, NULL, 0) == 0);
  TEST_CHECK(after.mtime_s == original_before.mtime_s &&
             after.mtime_ns == original_before.mtime_ns);

cleanup:
  free(original);
  string_buffer_free(&error_buffer);
  free(jpeg);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Freshness is decided separately: a fresh thumbnail remains untouched while a zero-byte large
// derivative and a same-age original of the wrong size are regenerated.
static void test_generate_refreshes_outputs_independently(void) {
  char root_dir_template[] = "/tmp/fram-derivative-freshness.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  struct MediaItem item;
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  struct GalleryConfig config;
  init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (init_item(&item, MEDIA_KIND_IMAGE, root_dir, "source.jpg", &arena) != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &jpeg, &jpeg_len) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.source_path, jpeg, jpeg_len, NULL, 0) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(derivative_renderer_generate(&config, media, 1, frames, 1, false, &error_buffer) == 0);
  struct FsMeta thumb_before;
  TEST_CHECK(fs_stat_meta(item.derivatives[0].path, &thumb_before, NULL, 0) == 0);

  if (!TEST_CHECK(fs_write_file(item.derivatives[2].path, "", 0, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.original_path, "x", 1, NULL, 0) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(derivative_renderer_generate(&config, media, 1, frames, 1, false, &error_buffer) == 0);

  struct FsMeta thumb_after;
  struct FsMeta large_after;
  struct FsMeta original_after;
  TEST_CHECK(fs_stat_meta(item.derivatives[0].path, &thumb_after, NULL, 0) == 0);
  TEST_CHECK(thumb_after.mtime_s == thumb_before.mtime_s &&
             thumb_after.mtime_ns == thumb_before.mtime_ns);
  TEST_CHECK(fs_stat_meta(item.derivatives[2].path, &large_after, NULL, 0) == 0);
  TEST_CHECK(large_after.size_len > 0);
  TEST_CHECK(fs_stat_meta(item.original_path, &original_after, NULL, 0) == 0);
  TEST_CHECK(original_after.size_len == jpeg_len);

cleanup:
  string_buffer_free(&error_buffer);
  free(jpeg);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// When both image derivatives and the original are fresh, the renderer never decodes the source.
// Replacing it with same-sized invalid bytes and backdating it therefore still succeeds.
static void test_generate_avoids_decode_when_derivatives_fresh(void) {
  char root_dir_template[] = "/tmp/fram-derivative-skip-decode.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  struct MediaItem item;
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  struct GalleryConfig config;
  init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  unsigned char* invalid = NULL;
  if (init_item(&item, MEDIA_KIND_IMAGE, root_dir, "source.jpg", &arena) != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &jpeg, &jpeg_len) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.source_path, jpeg, jpeg_len, NULL, 0) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(derivative_renderer_generate(&config, media, 1, frames, 1, false, &error_buffer) == 0);

  struct FsMeta thumb_meta;
  TEST_CHECK(fs_stat_meta(item.derivatives[0].path, &thumb_meta, NULL, 0) == 0);
  invalid = calloc(jpeg_len, 1);
  if (!TEST_CHECK(invalid != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.source_path, invalid, jpeg_len, NULL, 0) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(set_older_mtime(item.source_path, &thumb_meta) == 0);
  TEST_CHECK(derivative_renderer_generate(&config, media, 1, frames, 1, false, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);

cleanup:
  free(invalid);
  string_buffer_free(&error_buffer);
  free(jpeg);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Image sources apply EXIF before resizing. Orientation 6 turns a stored left/right color split
// into a presented top/bottom split. Checking both regions distinguishes it from a distorted
// resize.
static void test_generate_applies_image_exif_orientation(void) {
  char root_dir_template[] = "/tmp/fram-derivative-exif.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  struct MediaItem item;
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  struct GalleryConfig config;
  init_config(&config, 2, 4, 16, 16, 95);
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  unsigned char* output = NULL;
  size_t output_len = 0;
  struct Image decoded = {0};
  if (init_item(&item, MEDIA_KIND_IMAGE, root_dir, "oriented.jpg", &arena) != 0) {
    goto cleanup;
  }
  item.derivatives[0].width_px = 2;
  item.derivatives[0].height_px = 4;
  item.derivatives[2].width_px = 8;
  item.derivatives[2].height_px = 16;
  if (!TEST_CHECK(make_oriented_jpeg(&jpeg, &jpeg_len) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.source_path, jpeg, jpeg_len, NULL, 0) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(derivative_renderer_generate(&config, media, 1, frames, 1, false, &error_buffer) == 0);

  TEST_CHECK(fs_read_file(item.derivatives[2].path, TEST_FILE_LEN_MAX, &output, &output_len, NULL,
                          0) == 0);
  TEST_CHECK(image_decode(output, output_len, 0, 0, &decoded, NULL, 0) == 0);
  if (TEST_CHECK(decoded.width_px == 8 && decoded.height_px == 16)) {
    const size_t top = (2 * decoded.width_px + 4) * IMAGE_CHANNEL_COUNT;
    const size_t bottom = (13 * decoded.width_px + 4) * IMAGE_CHANNEL_COUNT;
    TEST_CHECK(decoded.pixels[top] > decoded.pixels[top + 2]);
    TEST_CHECK(decoded.pixels[bottom + 2] > decoded.pixels[bottom]);
  }

cleanup:
  image_free(&decoded);
  free(output);
  string_buffer_free(&error_buffer);
  free(jpeg);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A video uses its retained auto-rotated JPEG, copies the actual video original, and unlinks the
// frame after the job. A failed frame decode still copies the original and preserves that first
// decode diagnostic.
static void test_generate_video_unlinks_frame_on_success_and_failure(void) {
  char root_dir_template[] = "/tmp/fram-derivative-video.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  struct MediaItem item;
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  char* frame_path = path_join(root_dir, "frame.jpg", &arena);
  struct GalleryConfig config;
  init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  char* frames[] = {frame_path};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  const char* decode_prefix = "failed to decode derivative source: ";
  char frame_suffix[512];
  int frame_suffix_len = -1;
  unsigned char* original = NULL;
  size_t original_len = 0;
  if (init_item(&item, MEDIA_KIND_VIDEO, root_dir, "source.mp4", &arena) != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.source_path, "video-bytes", strlen("video-bytes"), NULL, 0) ==
                  0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &jpeg, &jpeg_len) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(frame_path, jpeg, jpeg_len, NULL, 0) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(derivative_renderer_generate(&config, media, 1, frames, 1, false, &error_buffer) == 0);
  TEST_CHECK(access(frame_path, F_OK) != 0);

  // Force all derivatives stale, then hand the next job an invalid retained frame.
  if (!TEST_CHECK(fs_write_file(item.derivatives[0].path, "", 0, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.derivatives[1].path, "", 0, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.derivatives[2].path, "", 0, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.original_path, "x", 1, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(frame_path, "bad", 3, NULL, 0) == 0)) {
    goto cleanup;
  }
  string_buffer_free(&error_buffer);
  string_buffer_init(&error_buffer);
  TEST_CHECK(derivative_renderer_generate(&config, media, 1, frames, 1, false, &error_buffer) ==
             -1);
  TEST_CHECK(access(frame_path, F_OK) != 0);
  // The decoder's own reason sits between the operation and the frame path, so only those two ends
  // are pinned.
  if (!TEST_CHECK(error_buffer.data != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strncmp(error_buffer.data, decode_prefix, strlen(decode_prefix)) == 0);
  frame_suffix_len = snprintf(frame_suffix, sizeof(frame_suffix), "('%s')", frame_path);
  if (!TEST_CHECK(frame_suffix_len > 0 && (size_t)frame_suffix_len < sizeof(frame_suffix))) {
    goto cleanup;
  }
  TEST_CHECK(
      error_buffer.len >= (size_t)frame_suffix_len &&
      strcmp(error_buffer.data + error_buffer.len - (size_t)frame_suffix_len, frame_suffix) == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");

  TEST_CHECK(
      fs_read_file(item.original_path, TEST_FILE_LEN_MAX, &original, &original_len, NULL, 0) == 0);
  TEST_CHECK(original_len == strlen("video-bytes"));
  TEST_CHECK(memcmp(original, "video-bytes", original_len) == 0);

cleanup:
  free(original);
  string_buffer_free(&error_buffer);
  free(jpeg);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A verbose phase prints one progress line per finished job, then closes the line. The lines are
// written from worker threads under `stderr`'s lock, which is the only hand-written locking outside
// the pool. This drives it with more workers than one so the lock is contended, and the `tsan` test
// preset runs the same path under ThreadSanitizer.
static void test_verbose_prints_one_progress_line_per_job(void) {
  char root_dir_template[] = "/tmp/fram-derivative-verbose.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  enum { ITEM_COUNT = 3 };
  struct Arena arena;
  arena_init(&arena);
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  struct MediaItem items[ITEM_COUNT];
  struct MediaItem* media[ITEM_COUNT];
  char* frames[ITEM_COUNT] = {NULL};
  static const char* const names[ITEM_COUNT] = {"a.jpg", "b.jpg", "c.jpg"};
  struct GalleryConfig config;
  init_config(&config, 2, 2, 8, 8, 82);
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  char stderr_out[128];
  int rc = -1;
  size_t progress_len = 0;
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &jpeg, &jpeg_len) == 0)) {
    goto cleanup;
  }
  for (size_t i = 0; i < ITEM_COUNT; i++) {
    if (init_item(&items[i], MEDIA_KIND_IMAGE, root_dir, names[i], &arena) != 0) {
      goto cleanup;
    }
    if (relocate_item_outputs(&items[i], root_dir, "public", names[i], &arena) != 0) {
      goto cleanup;
    }
    if (!TEST_CHECK(fs_write_file(items[i].source_path, jpeg, jpeg_len, NULL, 0) == 0)) {
      goto cleanup;
    }
    media[i] = &items[i];
  }

  rc = generate_capturing_stderr(&config, media, ITEM_COUNT, frames, 4, &error_buffer, stderr_out,
                                 sizeof(stderr_out));

  TEST_CHECK(rc == 0);
  TEST_CHECK(error_buffer.len == 0);
  // Three jobs, so three progress lines, and one trailing newline closing the line before any later
  // status message. Workers finish in any order, so each count is asserted present once rather than
  // the whole text in sequence.
  static const char* const progress_lines[] = {"\rgenerating derivatives 1/3",
                                               "\rgenerating derivatives 2/3",
                                               "\rgenerating derivatives 3/3"};
  for (size_t i = 0; i < sizeof(progress_lines) / sizeof(progress_lines[0]); i++) {
    const char* found = strstr(stderr_out, progress_lines[i]);
    TEST_CHECK(found != NULL && strstr(found + 1, progress_lines[i]) == NULL);
    progress_len += strlen(progress_lines[i]);
  }
  TEST_CHECK(strlen(stderr_out) == progress_len + 1);
  TEST_CHECK(stderr_out[progress_len] == '\n');
  TEST_MSG("stderr: '%s'", stderr_out);

cleanup:
  string_buffer_free(&error_buffer);
  free(jpeg);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Derivatives that share parent directories are written concurrently, each job creating the parents
// its own outputs need. Every job races its siblings to create `public/trip` and
// `public/trip/deep`, so a job that took a sibling's `EEXIST` for a failure would drop its
// derivatives. The `tsan` test preset runs the same path under ThreadSanitizer.
static void test_writes_derivatives_sharing_parents_concurrently(void) {
  char root_dir_template[] = "/tmp/fram-derivative-shared-parents.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  enum { ITEM_COUNT = 16 };
  struct Arena arena;
  arena_init(&arena);
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  struct MediaItem items[ITEM_COUNT];
  struct MediaItem* media[ITEM_COUNT];
  char* frames[ITEM_COUNT] = {NULL};
  struct GalleryConfig config;
  init_config(&config, 2, 2, 8, 8, 82);
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &jpeg, &jpeg_len) == 0)) {
    goto cleanup;
  }
  for (size_t i = 0; i < ITEM_COUNT; i++) {
    char name[64];
    const int name_len = snprintf(name, sizeof(name), "photos/trip/deep/one-%02zu.jpg", i);
    if (!TEST_CHECK(name_len > 0 && (size_t)name_len < sizeof(name))) {
      goto cleanup;
    }
    char stem[32];
    const int stem_len = snprintf(stem, sizeof(stem), "one-%02zu-jpg", i);
    if (!TEST_CHECK(stem_len > 0 && (size_t)stem_len < sizeof(stem))) {
      goto cleanup;
    }
    if (init_item(&items[i], MEDIA_KIND_IMAGE, root_dir, arena_strdup(&arena, name), &arena) != 0) {
      goto cleanup;
    }
    if (relocate_item_outputs(&items[i], root_dir, "public/trip/deep", stem, &arena) != 0) {
      goto cleanup;
    }
    if (!TEST_CHECK(fs_write_file(items[i].source_path, jpeg, jpeg_len, NULL, 0) == 0)) {
      goto cleanup;
    }
    media[i] = &items[i];
  }

  TEST_CHECK(derivative_renderer_generate(&config, media, ITEM_COUNT, frames, 8, false,
                                          &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");

  for (size_t i = 0; i < ITEM_COUNT; i++) {
    size_t width = 0;
    size_t height = 0;
    TEST_CHECK(probe_file(items[i].derivatives[0].path, &width, &height) == 0);
    TEST_CHECK(width == 2 && height == 2);
    TEST_CHECK(probe_file(items[i].derivatives[1].path, &width, &height) == 0);
    TEST_CHECK(width == 8 && height == 4);
    TEST_CHECK(probe_file(items[i].derivatives[2].path, &width, &height) == 0);
    TEST_CHECK(width == 8 && height == 4);
    TEST_CHECK(access(items[i].original_path, F_OK) == 0);
    TEST_MSG("item %zu", i);
  }

cleanup:
  string_buffer_free(&error_buffer);
  free(jpeg);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// The phase accepts an empty media set. `job_run` allocates error slots whose count is 0, for which
// `calloc(0, ...)` may return `NULL`, and it must not report that as an allocation failure.
static void test_accepts_empty_media_set(void) {
  struct GalleryConfig config;
  init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[1] = {NULL};
  char* frames[1] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(derivative_renderer_generate(&config, media, 0, frames, 1, false, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);

  string_buffer_free(&error_buffer);
}

// A read reason survives in full when a long source path overflows the diagnostic, and the overflow
// itself is marked rather than silent. The source and the fresh original are sparse, so the fixture
// costs no disk.
static void test_reports_read_reason_before_long_source_path(void) {
  char root_dir_template[] = "/tmp/fram-derivative-long-source.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  // Three 200-byte directories put the source path alone past `ERROR_MESSAGE_SIZE`, so the composed
  // message cannot fit and is cut.
  enum { SEGMENT_LEN = 200, DEPTH = 3 };
  char name[DEPTH * (SEGMENT_LEN + 1) + sizeof("source.jpg")];
  size_t name_len = 0;
  for (size_t i = 0; i < (size_t)DEPTH; i++) {
    memset(name + name_len, 'a', SEGMENT_LEN);
    name_len += SEGMENT_LEN;
    name[name_len++] = '/';
  }
  memcpy(name + name_len, "source.jpg", sizeof("source.jpg"));
  _Static_assert(DEPTH * (SEGMENT_LEN + 1) > ERROR_MESSAGE_SIZE,
                 "the source path must overflow the diagnostic");

  struct Arena arena;
  arena_init(&arena);
  struct MediaItem item;
  const off_t source_len = (off_t)IMAGE_INPUT_LEN_MAX + 1;
  struct GalleryConfig config;
  init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  char expected_head[160];
  int expected_head_len = -1;
  if (init_item(&item, MEDIA_KIND_IMAGE, root_dir, name, &arena) != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.source_path, "", 0, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(truncate(item.source_path, source_len) == 0)) {
    goto cleanup;
  }
  // An original of the same size written after the source is fresh, so the read failure is the
  // job's only diagnostic.
  if (!TEST_CHECK(fs_write_file(item.original_path, "", 0, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(truncate(item.original_path, source_len) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(derivative_renderer_generate(&config, media, 1, frames, 1, false, &error_buffer) ==
             -1);
  // The reason leads, so the clause naming the limit and the measured length is present even though
  // the composed message is longer than the buffer. The prefix assertion verifies this ordering.
  // The source path trails, so truncation removes the value rather than the reason.
  expected_head_len =
      snprintf(expected_head, sizeof(expected_head),
               "failed to read derivative source: exceeds max file size (%d bytes) at %jd bytes ('",
               IMAGE_INPUT_LEN_MAX, (intmax_t)source_len);
  if (!TEST_CHECK(expected_head_len > 0 && (size_t)expected_head_len < sizeof(expected_head))) {
    goto cleanup;
  }
  TEST_CHECK(error_buffer.data != NULL &&
             strncmp(error_buffer.data, expected_head, (size_t)expected_head_len) == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");

  // And the cut is marked. Filling the buffer exactly is what proves this case still truncates at
  // all: a reworded diagnostic that fits would leave the marker assertion below passing for the
  // wrong reason, so the length is asserted first and fails loudly instead. Without the marker a
  // cut path reads as a whole one and names a file that does not exist. The `...` is spelled out
  // because `TRUNCATION_MARKER` is file-local to `core/error.c`. Changing it there must update this
  // line.
  TEST_CHECK(error_buffer.len == ERROR_MESSAGE_SIZE - 1);
  TEST_CHECK(error_buffer.data != NULL && error_buffer.len >= 3 &&
             strcmp(error_buffer.data + error_buffer.len - 3, "...") == 0);

cleanup:
  string_buffer_free(&error_buffer);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A source that exists but cannot be read is reported with the system cause and the path it
// happened on, rather than as a decode failure. The derivatives are stale, so the job reaches the
// read, and the original is fresh, so the read failure is its only diagnostic.
static void test_reports_unreadable_source(void) {
  // Root bypasses the permission bits, so the source would read fine and the assertion below would
  // fail for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }

  char root_dir_template[] = "/tmp/fram-derivative-unreadable.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  struct MediaItem item;
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  struct GalleryConfig config;
  init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  int rc = 0;
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = -1;
  if (init_item(&item, MEDIA_KIND_IMAGE, root_dir, "source.jpg", &arena) != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &jpeg, &jpeg_len) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.source_path, jpeg, jpeg_len, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.original_path, jpeg, jpeg_len, NULL, 0) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(chmod(item.source_path, 0) == 0);

  rc = derivative_renderer_generate(&config, media, 1, frames, 1, false, &error_buffer);
  // Restore the mode right after the call, so the unreadable source never outlives what it tests.
  (void)chmod(item.source_path, 0644);

  TEST_CHECK(rc == -1);
  expected_len = snprintf(expected, sizeof(expected), "failed to read derivative source: %s ('%s')",
                          error_system_message(reason, sizeof(reason), EACCES), item.source_path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");

cleanup:
  string_buffer_free(&error_buffer);
  free(jpeg);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A derivative source larger than the decoder's `IMAGE_INPUT_LEN_MAX` input limit is rejected at
// the read, from its size, naming the limit and the size, rather than loaded whole and turned away
// by the decoder. The source and the fresh original are sparse, so the fixture costs no disk.
static void test_rejects_oversize_source(void) {
  char root_dir_template[] = "/tmp/fram-derivative-oversize.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  struct MediaItem item;
  const off_t source_len = (off_t)IMAGE_INPUT_LEN_MAX + 1;
  struct GalleryConfig config;
  init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  char* frames[] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = -1;
  if (init_item(&item, MEDIA_KIND_IMAGE, root_dir, "source.jpg", &arena) != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(item.source_path, "", 0, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(truncate(item.source_path, source_len) == 0)) {
    goto cleanup;
  }
  // An original of the same size written after the source is fresh, so the job reaches the
  // derivative read without first copying two gigabytes.
  if (!TEST_CHECK(fs_write_file(item.original_path, "", 0, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(truncate(item.original_path, source_len) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(derivative_renderer_generate(&config, media, 1, frames, 1, false, &error_buffer) ==
             -1);
  expected_len = snprintf(expected, sizeof(expected),
                          "failed to read derivative source: exceeds max file size (%d bytes) at "
                          "%jd bytes ('%s')",
                          IMAGE_INPUT_LEN_MAX, (intmax_t)source_len, item.source_path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");

cleanup:
  string_buffer_free(&error_buffer);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Two failing sources append one diagnostic line each, separated by exactly one newline, with none
// leading or trailing the buffer. That separator placement is what the "one per line" contract in
// `derivative_renderer.h` means, and a single-source failure cannot observe it at all.
// `test_reports_one_line_per_failing_media_item` in `src/app/test_cmd_build.c` pins the same
// separator for failures in the probe phase, which stops a build before this phase runs. This one
// pins it at the module boundary, where the contract is documented.
static void test_appends_one_error_line_per_failing_source(void) {
  char root_dir_template[] = "/tmp/fram-derivative-error-lines.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  // Both sources are not JPEG data, so each fails its decode with its own message.
  struct Arena arena;
  arena_init(&arena);
  struct MediaItem items[2];
  static const char* const names[] = {"a.jpg", "b.jpg"};
  struct GalleryConfig config;
  init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&items[0], &items[1]};
  char* frames[] = {NULL, NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  char expected[2 * ERROR_MESSAGE_SIZE];
  int expected_len = -1;
  for (size_t i = 0; i < 2; i++) {
    if (init_item(&items[i], MEDIA_KIND_IMAGE, root_dir, names[i], &arena) != 0) {
      goto cleanup;
    }
    if (relocate_item_outputs(&items[i], root_dir, "public", names[i], &arena) != 0) {
      goto cleanup;
    }
    if (!TEST_CHECK(fs_write_file(items[i].source_path, "not a JPEG\n", strlen("not a JPEG\n"),
                                  NULL, 0) == 0)) {
      goto cleanup;
    }
  }

  TEST_CHECK(derivative_renderer_generate(&config, media, 2, frames, 2, false, &error_buffer) ==
             -1);
  // Compared whole, with the separator in the middle: a dropped separator, a lost line, or a
  // leading or trailing newline each change these bytes.
  expected_len =
      snprintf(expected, sizeof(expected),
               "failed to decode derivative source: failed to probe JPEG: unknown image type "
               "('%s')\n"
               "failed to decode derivative source: failed to probe JPEG: unknown image type "
               "('%s')",
               items[0].source_path, items[1].source_path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");

cleanup:
  string_buffer_free(&error_buffer);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"generate image and skip fresh outputs", test_generate_image_and_skip_fresh_outputs},
    {"generate refreshes outputs independently", test_generate_refreshes_outputs_independently},
    {"generate avoids decode when derivatives fresh",
     test_generate_avoids_decode_when_derivatives_fresh},
    {"generate applies image exif orientation", test_generate_applies_image_exif_orientation},
    {"generate video unlinks frame on success and failure",
     test_generate_video_unlinks_frame_on_success_and_failure},
    {"verbose prints one progress line per job", test_verbose_prints_one_progress_line_per_job},
    {"writes derivatives sharing parents concurrently",
     test_writes_derivatives_sharing_parents_concurrently},
    {"accepts empty media set", test_accepts_empty_media_set},
    {"reports read reason before long source path",
     test_reports_read_reason_before_long_source_path},
    {"reports unreadable source", test_reports_unreadable_source},
    {"rejects oversize source", test_rejects_oversize_source},
    {"appends one error line per failing source", test_appends_one_error_line_per_failing_source},
    {NULL, NULL},
};
