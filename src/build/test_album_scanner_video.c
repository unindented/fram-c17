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
#include "build/job.h"
#include "build/test_album_scanner.h"
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

// The video cases of the probe phase. `ffprobe` and `ffmpeg` are fake scripts on `PATH`, and the
// frame they write is a file on the real host, so these run against the POSIX one. The other probe
// and skeleton cases are in `test_album_scanner.c`, on the in-memory host.

/** Largest file a test here reads back, in bytes. Every fixture file is at most a few kilobytes. */
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
  static const struct Str probe_script = STR_INIT("#!/bin/sh\nprintf '0.400\\n'\n");
  static const struct Str frame_script = STR_INIT(
      "#!/bin/sh\n"
      "previous=\n"
      "last=\n"
      "for arg do\n"
      "  if [ \"$previous\" = -ss ]; then printf '%s' \"$arg\" > \"$FRAM_TEST_LOG\"; fi\n"
      "  previous=$arg\n"
      "  last=$arg\n"
      "done\n"
      "/bin/cp \"$FRAM_TEST_FRAME\" \"$last\"\n");
  struct Str probe_path = fixture_path(root_dir, "ffprobe", arena);
  struct Str frame_path = fixture_path(root_dir, "ffmpeg", arena);
  if (write_fixture_bytes(probe_path, probe_script) != 0 ||
      write_fixture_bytes(frame_path, frame_script) != 0 || chmod(probe_path.data, 0700) != 0 ||
      chmod(frame_path.data, 0700) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  return 0;
}

/** The environment the fake video tools read, as it was before a test pointed it at a fixture. */
struct FakeVideoEnv {
  /** `PATH` before the test, or `NULL` when it was unset. */
  const char* path_saved;

  /** `TMPDIR` before the test, or `NULL` when it was unset. */
  const char* tmpdir_saved;

  /** `FRAM_TEST_FRAME` before the test, or `NULL` when it was unset. */
  const char* frame_saved;

  /** `FRAM_TEST_LOG` before the test, or `NULL` when it was unset. */
  const char* log_saved;
};

/**
 * @brief Saves the environment the fake video tools read, then points it at a fixture directory.
 *
 * `PATH` and `TMPDIR` become `root_dir`, so the probe runs the scripts `write_fake_video_tools`
 * writes there and its frame lands there too. `FRAM_TEST_FRAME` names the file the fake `ffmpeg`
 * copies, and `FRAM_TEST_LOG` the file it writes its `-ss` argument to.
 *
 * @param root_dir      Terminated fixture directory. Must not be `NULL`.
 * @param frame_fixture Frame the fake `ffmpeg` copies, its `data` terminated.
 * @param log_path      File the fake `ffmpeg` logs its seek to, its `data` terminated.
 * @param arena         Arena that owns the saved values. Must not be `NULL`.
 * @param env_out       Receives the saved values for `fake_video_env_leave`. Must not be `NULL`.
 */
static void fake_video_env_enter(const char* root_dir,
                                 struct Str frame_fixture,
                                 struct Str log_path,
                                 struct Arena* arena,
                                 struct FakeVideoEnv* env_out) {
  *env_out = (struct FakeVideoEnv){
      .path_saved = save_env("PATH", arena),
      .tmpdir_saved = save_env("TMPDIR", arena),
      .frame_saved = save_env("FRAM_TEST_FRAME", arena),
      .log_saved = save_env("FRAM_TEST_LOG", arena),
  };
  TEST_CHECK(setenv("PATH", root_dir, 1) == 0);
  TEST_CHECK(setenv("TMPDIR", root_dir, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_FRAME", frame_fixture.data, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_LOG", log_path.data, 1) == 0);
}

/**
 * @brief Puts back the environment `fake_video_env_enter` saved.
 *
 * @param env Values `fake_video_env_enter` saved. Must not be `NULL`.
 */
static void fake_video_env_leave(const struct FakeVideoEnv* env) {
  restore_env("PATH", env->path_saved);
  restore_env("TMPDIR", env->tmpdir_saved);
  restore_env("FRAM_TEST_FRAME", env->frame_saved);
  restore_env("FRAM_TEST_LOG", env->log_saved);
}

// A short video seeks to its midpoint and leaves one auto-rotated frame in its slot, which the
// cleanup removes.
static void test_probe_media_retains_clamped_video_frame(void) {
  char root_dir[] = "/tmp/fram-scanner-video.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  struct Str frame_fixture = fixture_path(root_dir, "frame.jpg", &arena);
  struct Str log_path = fixture_path(root_dir, "seek.log", &arena);
  struct Str jpeg = {0};
  struct FakeVideoEnv env;
  fake_video_env_enter(root_dir, frame_fixture, log_path, &arena, &env);
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  struct MediaDerivative derivatives[3] = {0};
  struct MediaItem item = {.source_path = S("short.mp4"),
                           .kind = MEDIA_KIND_VIDEO,
                           .derivatives = derivatives,
                           .derivative_count = 3};
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct JobArenas* worker_arenas = NULL;
  struct Log log = test_log();
  struct Log read_log = test_log();
  struct Str logged = {0};
  if (write_fake_video_tools(root_dir, &arena) != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(test_jpeg_encode(2, 3, &arena, &jpeg) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(write_fixture_bytes(frame_fixture, jpeg) == 0);

  video_frame_init(&frames[0], &arena);

  worker_arenas = test_job_arenas(1, &arena);
  TEST_CHECK(album_scanner_probe_media(&config, media, 1, frames, 1, worker_arenas, false,
                                       os_posix(), arena, &log) == 0);
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
  if (!TEST_CHECK(frames[0].is_created)) {
    goto cleanup;
  }
  TEST_CHECK(access(frames[0].path, F_OK) == 0);

  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_read(os_posix(), log_path, TEST_FILE_LEN_MAX, &arena, &logged,
                     S("failed to read fixture"), &read_log) == 0);
  TEST_CHECK(str_eq(logged, S("0.200")));
  album_scanner_remove_video_frames(frames, 1);
  TEST_CHECK(!frames[0].is_created);
  TEST_CHECK(access(frames[0].path, F_OK) != 0);

cleanup:
  album_scanner_remove_video_frames(frames, 1);
  fake_video_env_leave(&env);
  remove_fixture_tree(root_dir);
}

// A frame that is not a JPEG fails its video's probe, and the failure names the video rather than
// the temporary frame, which is gone once the build ends.
static void test_probe_media_names_video_of_invalid_frame(void) {
  char root_dir[] = "/tmp/fram-scanner-frame-invalid.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  struct Str frame_fixture = fixture_path(root_dir, "frame.jpg", &arena);
  struct Str log_path = fixture_path(root_dir, "seek.log", &arena);
  struct FakeVideoEnv env;
  fake_video_env_enter(root_dir, frame_fixture, log_path, &arena, &env);
  struct GalleryConfig config;
  test_album_scanner_init_config(&config);
  config.derivative_count = 0;
  struct MediaItem item = {.source_path = S("short.mp4"), .kind = MEDIA_KIND_VIDEO};
  struct MediaItem* media[] = {&item};
  struct VideoFrame frames[1] = {{0}};
  struct JobArenas* worker_arenas = NULL;
  struct Log log = test_log();
  if (write_fake_video_tools(root_dir, &arena) != 0) {
    goto cleanup;
  }
  TEST_CHECK(write_fixture_bytes(frame_fixture, S("not a jpeg")) == 0);

  video_frame_init(&frames[0], &arena);

  worker_arenas = test_job_arenas(1, &arena);
  TEST_CHECK(album_scanner_probe_media(&config, media, 1, frames, 1, worker_arenas, false,
                                       os_posix(), arena, &log) == -1);
  TEST_CHECK(str_startswith(buf_str(&log.buf), S("failed to probe media: failed to probe JPEG: ")));
  TEST_CHECK(str_endswith(buf_str(&log.buf), S(" ('short.mp4')\n")));
  TEST_CHECK(log.errors == 1);
  TEST_MSG("log: '%s'", buf_cstr(&log.buf));

cleanup:
  album_scanner_remove_video_frames(frames, 1);
  fake_video_env_leave(&env);
  remove_fixture_tree(root_dir);
}

// The cleanup unlinks exactly the frames a slot still marks as created. It leaves alone a slot
// whose frame was never created, whose template may name another file, and a slot whose frame the
// derivative job already removed, whose name another process may have reused.
static void test_remove_video_frames_unlinks_only_created(void) {
  char root_dir[] = "/tmp/fram-scanner-frames.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  struct VideoFrame frames[] = {
      {.path = str_cstr(fixture_path(root_dir, "created.jpg", &arena), &arena), .is_created = true},
      {.path = str_cstr(fixture_path(root_dir, "frame.XXXXXX", &arena), &arena),
       .is_created = false},
      {.path = str_cstr(fixture_path(root_dir, "reused.jpg", &arena), &arena), .is_created = false},
      {.path = NULL, .is_created = false},
  };
  for (ptrdiff_t i = 0; i < 3; i++) {
    TEST_CHECK(write_fixture_bytes(str_from_cstr(frames[i].path), S("x")) == 0);
  }

  album_scanner_remove_video_frames(frames, (ptrdiff_t)(sizeof(frames) / sizeof(frames[0])));

  TEST_CHECK(!frames[0].is_created);
  TEST_CHECK(access(frames[0].path, F_OK) != 0);
  TEST_CHECK(access(frames[1].path, F_OK) == 0);
  TEST_CHECK(access(frames[2].path, F_OK) == 0);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"probe media retains clamped video frame", test_probe_media_retains_clamped_video_frame},
    {"probe media names video of invalid frame", test_probe_media_names_video_of_invalid_frame},
    {"remove video frames unlinks only created", test_remove_video_frames_unlinks_only_created},
    {NULL, NULL},
};
