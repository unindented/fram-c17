#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "build/derivative_renderer.h"
#include "build/job.h"
#include "build/output_writer.h"
#include "build/test_derivative_renderer.h"
#include "build/test_job_arenas.h"
#include "core/path.h"
#include "domain/gallery_config.h"
#include "domain/media_item.h"
#include "formats/image.h"
#include "runtime/fs.h"
#include "runtime/os.h"
#include "runtime/video.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_jpeg_fixture.h"
#include "test_support.h"

// The video case of the derivative phase. A video's frame is a file `ffmpeg` wrote on the real
// host, so this runs against the POSIX one. The image cases are in `test_derivative_renderer.c`, on
// the in-memory host.

/** Largest file a test here reads back, in bytes. Fixture JPEGs are under a kilobyte. */
enum { TEST_FILE_LEN_MAX = 1024 * 1024 };

/**
 * @brief Joins a relative path onto a fixture directory, keeping a terminator after it.
 *
 * The terminator lets a test hand `data` straight to a system call that sets up the fixture, while
 * the calls under test read the counted string.
 *
 * @param root_dir      Terminated fixture directory. Must not be `NULL`.
 * @param relative_path Terminated path below `root_dir`. Must not be `NULL`.
 * @param arena         Arena that owns the joined path. Must not be `NULL`.
 * @return The joined path, its `data` terminated.
 */
static struct Str fixture_path(const char* root_dir,
                               const char* relative_path,
                               struct Arena* arena) {
  const struct Str path = path_join(str_from_cstr(root_dir), str_from_cstr(relative_path), arena);
  return (struct Str){str_cstr(path, arena), path.len};
}

/**
 * @brief Writes one fixture file, failing the current test check when it cannot.
 *
 * @param path     Path of the file to write.
 * @param contents Bytes to write.
 * @return `0` on success, or `-1` after failing the current test check.
 */
static int write_fixture_bytes(struct Str path, struct Str contents) {
  struct Log log = test_log();
  const int rc = os_write(os_posix(), path, (struct OsSource){.bytes = contents},
                          S("failed to write fixture"), &log);
  const struct Str failure = buf_str(&log.buf);
  TEST_CHECK_(rc == 0, "fixture '%.*s' is written", STR_FMT(path));
  TEST_MSG("%.*s", STR_FMT(failure));
  return rc;
}

/**
 * @brief Populates one media item with three derivatives and an original below `root_dir`.
 *
 * The derivatives are named `s`, `m`, and `l` and match the ones
 * `test_derivative_renderer_init_config` configures.
 *
 * @param item     Item to populate. Must not be `NULL`.
 * @param kind     Media kind of the item.
 * @param root_dir Fixture root the source and output paths are joined under. Must not be `NULL`.
 * @param name     Source filename relative to `root_dir`, also used as the title. Must not be
 *                 `NULL`.
 * @param arena    Arena that owns the item's derivatives and paths. Must not be `NULL`.
 */
static void init_item(struct MediaItem* item,
                      enum MediaKind kind,
                      const char* root_dir,
                      const char* name,
                      struct Arena* arena) {
  struct MediaDerivative* derivatives = arena_new(arena, 3, struct MediaDerivative);
  derivatives[0] = (struct MediaDerivative){
      .name = S("s"),
      .path = fixture_path(root_dir, "public/s.webp", arena),
      .width_px = 2,
      .height_px = 2,
  };
  derivatives[1] = (struct MediaDerivative){
      .name = S("m"),
      .path = fixture_path(root_dir, "public/m.webp", arena),
      .width_px = 8,
      .height_px = 4,
  };
  derivatives[2] = (struct MediaDerivative){
      .name = S("l"),
      .path = fixture_path(root_dir, "public/l.webp", arena),
      .width_px = 8,
      .height_px = 4,
  };
  *item = (struct MediaItem){
      .source_path = fixture_path(root_dir, name, arena),
      .title = str_clone(str_from_cstr(name), arena),
      .kind = kind,
      .derivatives = derivatives,
      .derivative_count = 3,
      .original_path = fixture_path(root_dir, "public/original.bin", arena),
      .orientation = IMAGE_ORIENTATION_TOP_LEFT,
      .width_px = 8,
      .height_px = 4,
  };
}

/**
 * @brief Runs a quiet derivative phase over fresh worker arenas, reading sources up to the media
 *        read limit.
 *
 * @param gallery_config Configured derivatives shared by every job. Must not be `NULL`.
 * @param media          Media items, one job per item. Must not be `NULL`.
 * @param media_count    Number of items in `media`.
 * @param video_frames   Video frame slots, one per item in `media`. Must not be `NULL`.
 * @param worker_count   Number of worker threads to request.
 * @param log            Log that receives derivative errors. Must not be `NULL`.
 * @return The renderer result.
 */
static int generate(const struct GalleryConfig* gallery_config,
                    struct MediaItem* const* media,
                    ptrdiff_t media_count,
                    struct VideoFrame* video_frames,
                    ptrdiff_t worker_count,
                    struct Log* log) {
  struct Arena arenas_perm = test_arena();
  struct Manifest manifest = {0};
  test_derivative_renderer_record_outputs(&manifest, media, media_count, &arenas_perm);
  const struct OutputWriter writer = {.manifest = &manifest, .os = os_posix()};
  return derivative_renderer_generate(&writer, gallery_config, media, media_count, video_frames,
                                      worker_count, test_job_arenas(worker_count, &arenas_perm),
                                      false, os_posix(), test_arena(), log);
}

// A video uses its retained auto-rotated JPEG, copies the actual video original, and unlinks the
// frame after the job. A failed frame decode still copies the original and preserves that first
// decode diagnostic.
static void test_generate_video_unlinks_frame_on_success_and_failure(void) {
  struct Log fixture_log = test_log();
  char root_dir[] = "/tmp/fram-derivative-video.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct MediaItem item;
  struct Str jpeg = {0};
  const struct Str frame_path = fixture_path(root_dir, "frame.jpg", &arena);
  struct GalleryConfig config;
  test_derivative_renderer_init_config(&config, 2, 2, 8, 8, 82);
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[] = {{.path = str_cstr(frame_path, &arena), .is_created = true}};
  struct Log log = test_log();
  struct Log failure_log = test_log();
  struct Str original = {0};
  init_item(&item, MEDIA_KIND_VIDEO, root_dir, "source.mp4", &arena);
  if (write_fixture_bytes(item.source_path, S("video-bytes")) != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(test_jpeg_encode(8, 4, &arena, &jpeg) == 0)) {
    goto cleanup;
  }
  if (write_fixture_bytes(frame_path, jpeg) != 0) {
    goto cleanup;
  }

  TEST_CHECK(generate(&config, media, 1, frames, 1, &log) == 0);
  TEST_CHECK(!frames[0].is_created);
  TEST_CHECK(access(frame_path.data, F_OK) != 0);

  // Force all derivatives stale, then hand the next job an invalid retained frame.
  for (ptrdiff_t i = 0; i < item.derivative_count; i++) {
    if (write_fixture_bytes(item.derivatives[i].path, S("")) != 0) {
      goto cleanup;
    }
  }
  if (write_fixture_bytes(item.original_path, S("x")) != 0 ||
      write_fixture_bytes(frame_path, S("bad")) != 0) {
    goto cleanup;
  }
  frames[0].is_created = true;
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(generate(&config, media, 1, frames, 1, &failure_log) == -1);
  TEST_CHECK(!frames[0].is_created);
  TEST_CHECK(access(frame_path.data, F_OK) != 0);
  // The decoder's own reason sits between the operation and the frame path, so only those two ends
  // are pinned, and the job reports nothing else.
  {
    const struct Str failure = buf_str(&failure_log.buf);
    const struct Str frame_suffix =
        str_concat(S(" ('"), str_concat(frame_path, S("')\n"), &arena), &arena);
    TEST_CHECK(
        str_startswith(failure, S("failed to decode derivative source: failed to probe JPEG: ")));
    TEST_CHECK(str_endswith(failure, frame_suffix));
    TEST_CHECK(failure_log.errors == 1);
    TEST_MSG("log: '%.*s'", STR_FMT(failure));
  }

  TEST_CHECK(os_read(os_posix(), item.original_path, TEST_FILE_LEN_MAX, &arena, &original,
                     S("failed to set up fixture"), &fixture_log) == 0);
  TEST_CHECK(str_eq(original, S("video-bytes")));

cleanup:
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"generate video unlinks frame on success and failure",
     test_generate_video_unlinks_frame_on_success_and_failure},
    {NULL, NULL},
};
