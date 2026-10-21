#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/path.h"
#include "runtime/fs.h"
#include "runtime/os.h"
#include "runtime/video.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few hundred bytes. */
enum { TEST_FILE_LEN_MAX = 1024 * 1024 };

/**
 * @brief Joins a literal relative path onto a fixture directory, as a terminated string.
 *
 * @param root_dir      Terminated fixture directory. Must not be `NULL`.
 * @param relative_path Terminated path below `root_dir`. Must not be `NULL`.
 * @param arena         Arena that owns the joined path. Must not be `NULL`.
 * @return The joined path, terminated, for the system calls and frame slots that take one.
 */
static char* fixture_cstr(const char* root_dir, const char* relative_path, struct Arena* arena)
    __attribute__((nonnull(1, 2, 3), returns_nonnull));

/** Writes executable fake FFmpeg tools into one test directory. */
static int write_fake_tools(const char* root_dir, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

// A slot whose frame was never created, such as one whose `mkostemp` failed, names no file of this
// build, so removing it leaves alone a file that happens to carry its template's name.
static void test_frame_remove_skips_uncreated_slot(void) {
  char root_dir[] = "/tmp/fram-video-remove-none.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  const char* tmpdir_saved = save_env("TMPDIR", &arena);
  TEST_CHECK(setenv("TMPDIR", root_dir, 1) == 0);
  struct VideoFrame frame;
  video_frame_init(&frame, &arena);
  restore_env("TMPDIR", tmpdir_saved);
  struct Log log = test_log();
  TEST_CHECK(os_write(os_posix(), str_from_cstr(frame.path), (struct OsSource){.bytes = S("other")},
                      S("failed to write fixture"), &log) == 0);

  video_frame_remove(&frame);

  TEST_CHECK(!frame.is_created);
  TEST_CHECK(access(frame.path, F_OK) == 0);
  remove_fixture_tree(root_dir);
}

// Once a frame is removed, its name may be reused by another process, so a second removal, as the
// end-of-build cleanup makes, leaves the new file alone.
static void test_frame_remove_skips_removed_frame(void) {
  char root_dir[] = "/tmp/fram-video-remove-twice.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  struct VideoFrame frame = {.path = fixture_cstr(root_dir, "frame.jpg", &arena),
                             .is_created = true};
  struct Log log = test_log();
  TEST_CHECK(os_write(os_posix(), str_from_cstr(frame.path), (struct OsSource){.bytes = S("frame")},
                      S("failed to write fixture"), &log) == 0);

  video_frame_remove(&frame);
  TEST_CHECK(!frame.is_created);
  TEST_CHECK(access(frame.path, F_OK) != 0);
  TEST_CHECK(os_write(os_posix(), str_from_cstr(frame.path),
                      (struct OsSource){.bytes = S("reused")}, S("failed to write fixture"),
                      &log) == 0);
  video_frame_remove(&frame);

  TEST_CHECK(access(frame.path, F_OK) == 0);
  remove_fixture_tree(root_dir);
}

// A frame that is already gone counts as removed, so the slot is cleared.
static void test_frame_remove_clears_missing_frame(void) {
  char root_dir[] = "/tmp/fram-video-remove-missing.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  struct VideoFrame frame = {.path = fixture_cstr(root_dir, "gone.jpg", &arena),
                             .is_created = true};

  video_frame_remove(&frame);

  TEST_CHECK(!frame.is_created);
  remove_fixture_tree(root_dir);
}

// An unlink that fails for a reason other than a missing file leaves the slot marked, so a later
// cleanup tries again and removes the frame once the unlink can succeed. A directory in the frame's
// place makes `unlink` fail even for a privileged user.
static void test_frame_remove_retries_failed_unlink(void) {
  char root_dir[] = "/tmp/fram-video-remove-retry.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  struct VideoFrame frame = {.path = fixture_cstr(root_dir, "frame.jpg", &arena),
                             .is_created = true};
  TEST_CHECK(mkdir(frame.path, 0700) == 0);

  video_frame_remove(&frame);
  TEST_CHECK(frame.is_created);

  TEST_CHECK(rmdir(frame.path) == 0);
  struct Log log = test_log();
  TEST_CHECK(os_write(os_posix(), str_from_cstr(frame.path), (struct OsSource){.bytes = S("frame")},
                      S("failed to write fixture"), &log) == 0);
  video_frame_remove(&frame);

  TEST_CHECK(!frame.is_created);
  TEST_CHECK(access(frame.path, F_OK) != 0);
  remove_fixture_tree(root_dir);
}

// Decimal seconds from ffprobe are parsed without locale dependence, padded or truncated to three
// fractional digits, and published only after a valid complete value.
static void test_probe_duration_parses_milliseconds(void) {
  char root_dir[] = "/tmp/fram-video-probe.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  if (write_fake_tools(root_dir, &arena) != 0) {
    remove_fixture_tree(root_dir);
    return;
  }

  const char* path_saved = save_env("PATH", &arena);
  const char* mode_saved = save_env("FRAM_TEST_PROBE_MODE", &arena);
  TEST_CHECK(setenv("PATH", root_dir, 1) == 0);

  struct Log log = test_log();
  ptrdiff_t duration_ms = 99;
  TEST_CHECK(setenv("FRAM_TEST_PROBE_MODE", "long", 1) == 0);
  TEST_CHECK(video_probe_duration(S("name with ; metacharacters.mp4"), arena, &duration_ms,
                                  S("failed to probe media"), &log) == 0);
  TEST_CHECK(duration_ms == 12345);

  TEST_CHECK(setenv("FRAM_TEST_PROBE_MODE", "short", 1) == 0);
  TEST_CHECK(video_probe_duration(S("video.mp4"), arena, &duration_ms, S("failed to probe media"),
                                  &log) == 0);
  TEST_CHECK(duration_ms == 400);

  TEST_CHECK(setenv("FRAM_TEST_PROBE_MODE", "unknown", 1) == 0);
  duration_ms = 77;
  TEST_CHECK(video_probe_duration(S("video.mp4"), arena, &duration_ms, S("failed to probe media"),
                                  &log) == 0);
  TEST_CHECK(duration_ms == 0);

  TEST_CHECK(setenv("FRAM_TEST_PROBE_MODE", "invalid", 1) == 0);
  duration_ms = 77;
  TEST_CHECK(test_log_is_empty(&log));
  struct Log invalid_log = test_log();
  TEST_CHECK(video_probe_duration(S("video.mp4"), arena, &duration_ms, S("failed to probe media"),
                                  &invalid_log) == -1);
  TEST_CHECK(duration_ms == 77);
  TEST_CHECK(test_log_is(&invalid_log,
                         "failed to probe media: ffprobe returned an invalid duration "
                         "('video.mp4')\n"));

  restore_env("PATH", path_saved);
  restore_env("FRAM_TEST_PROBE_MODE", mode_saved);
  remove_fixture_tree(root_dir);
}

// The longest duration a `ptrdiff_t` holds in milliseconds is accepted. One millisecond more is too
// large, whether the whole seconds alone overflow or only the seconds and milliseconds together, so
// neither wraps into a short duration. Every value is derived from `PTRDIFF_MAX`.
static void test_probe_duration_rejects_overflow(void) {
  char at_max[32];
  char past_total[32];
  char past_seconds[32];
  const int at_max_len =
      snprintf(at_max, sizeof(at_max), "%td.%03td", PTRDIFF_MAX / 1000, PTRDIFF_MAX % 1000);
  const int past_total_len = snprintf(past_total, sizeof(past_total), "%td.%03td",
                                      PTRDIFF_MAX / 1000, PTRDIFF_MAX % 1000 + 1);
  const int past_seconds_len = snprintf(past_seconds, sizeof(past_seconds), "%td0", PTRDIFF_MAX);
  TEST_ASSERT(at_max_len > 0 && (size_t)at_max_len < sizeof(at_max));
  TEST_ASSERT(past_total_len > 0 && (size_t)past_total_len < sizeof(past_total));
  TEST_ASSERT(past_seconds_len > 0 && (size_t)past_seconds_len < sizeof(past_seconds));
  char root_dir[] = "/tmp/fram-video-probe-max.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  if (write_fake_tools(root_dir, &arena) != 0) {
    remove_fixture_tree(root_dir);
    return;
  }

  const char* path_saved = save_env("PATH", &arena);
  const char* mode_saved = save_env("FRAM_TEST_PROBE_MODE", &arena);
  const char* output_saved = save_env("FRAM_TEST_PROBE_OUTPUT", &arena);
  TEST_CHECK(setenv("PATH", root_dir, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_PROBE_MODE", "output", 1) == 0);

  struct Log log = test_log();
  ptrdiff_t duration_ms = 0;
  TEST_CHECK(setenv("FRAM_TEST_PROBE_OUTPUT", at_max, 1) == 0);
  TEST_CHECK(video_probe_duration(S("video.mp4"), arena, &duration_ms, S("failed to probe media"),
                                  &log) == 0);
  TEST_CHECK(duration_ms == PTRDIFF_MAX);

  const char* const past_outputs[] = {past_total, past_seconds};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(past_outputs) / sizeof(past_outputs[0])); i++) {
    TEST_CHECK(setenv("FRAM_TEST_PROBE_OUTPUT", past_outputs[i], 1) == 0);
    duration_ms = 77;
    struct Log past_log = test_log();
    TEST_CHECK(video_probe_duration(S("video.mp4"), arena, &duration_ms, S("failed to probe media"),
                                    &past_log) == -1);
    TEST_CHECK(duration_ms == 77);
    TEST_CHECK(test_log_is(&past_log,
                           "failed to probe media: video duration is too large ('video.mp4')\n"));
    TEST_MSG("output: '%s'", past_outputs[i]);
  }

  restore_env("PATH", path_saved);
  restore_env("FRAM_TEST_PROBE_MODE", mode_saved);
  restore_env("FRAM_TEST_PROBE_OUTPUT", output_saved);
  remove_fixture_tree(root_dir);
}

// `ffprobe` output holding a `NUL` byte is an invalid duration, not the number before the byte,
// because the probe parses every byte the tool printed.
static void test_probe_duration_rejects_output_with_nul(void) {
  char root_dir[] = "/tmp/fram-video-probe-nul.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  if (write_fake_tools(root_dir, &arena) != 0) {
    remove_fixture_tree(root_dir);
    return;
  }

  const char* path_saved = save_env("PATH", &arena);
  const char* mode_saved = save_env("FRAM_TEST_PROBE_MODE", &arena);
  TEST_CHECK(setenv("PATH", root_dir, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_PROBE_MODE", "nul", 1) == 0);

  ptrdiff_t duration_ms = 77;
  struct Log log = test_log();
  TEST_CHECK(video_probe_duration(S("video.mp4"), arena, &duration_ms, S("failed to probe media"),
                                  &log) == -1);
  TEST_CHECK(duration_ms == 77);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: ffprobe returned an invalid duration "
                         "('video.mp4')\n"));

  restore_env("PATH", path_saved);
  restore_env("FRAM_TEST_PROBE_MODE", mode_saved);
  remove_fixture_tree(root_dir);
}

// A video path holding a `NUL` is refused before a tool runs, because the argument a child process
// receives would end at that byte and name another file.
static void test_video_rejects_path_with_nul(void) {
  struct Arena arena = test_arena();
  ptrdiff_t duration_ms = 77;
  struct Log probe_log = test_log();
  TEST_CHECK(video_probe_duration(S("a\0b.mp4"), arena, &duration_ms, S("failed to probe media"),
                                  &probe_log) == -1);
  TEST_CHECK(duration_ms == 77);
  TEST_CHECK(test_log_is(
      &probe_log, "failed to probe media: video path contains an embedded NUL byte ('a')\n"));

  struct VideoFrame frame = {.path = NULL};
  struct Log extract_log = test_log();
  TEST_CHECK(video_extract_frame(&frame, S("a\0b.mp4"), 0, arena, S("failed to probe media"),
                                 &extract_log) == -1);
  TEST_CHECK(!frame.is_created);
  TEST_CHECK(test_log_is(
      &extract_log, "failed to probe media: video path contains an embedded NUL byte ('a')\n"));
}

// Frame extraction passes a literal input path, formats the seek timestamp, and fills the slot's
// template in `TMPDIR` with a nonempty secure temporary that stays until the slot is removed.
static void test_extract_frame_fills_slot_temp(void) {
  char root_dir[] = "/tmp/fram-video-frame-test.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  if (write_fake_tools(root_dir, &arena) != 0) {
    remove_fixture_tree(root_dir);
    return;
  }
  char* temp_dir = fixture_cstr(root_dir, "tmp", &arena);
  char* log_path = fixture_cstr(root_dir, "frame.log", &arena);
  struct Log log = test_log();
  TEST_CHECK(os_mkdirs(os_posix(), str_from_cstr(temp_dir), S("failed to create fixture"), &log) ==
             0);

  const char* path_saved = save_env("PATH", &arena);
  const char* tmpdir_saved = save_env("TMPDIR", &arena);
  const char* log_saved = save_env("FRAM_TEST_LOG", &arena);
  const char* mode_saved = save_env("FRAM_TEST_FRAME_MODE", &arena);
  TEST_CHECK(setenv("PATH", root_dir, 1) == 0);
  TEST_CHECK(setenv("TMPDIR", temp_dir, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_LOG", log_path, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_FRAME_MODE", "success", 1) == 0);

  struct VideoFrame frame;
  video_frame_init(&frame, &arena);
  struct OsStat st;
  TEST_CHECK(strncmp(frame.path, temp_dir, strlen(temp_dir)) == 0);
  TEST_CHECK(video_extract_frame(&frame, S("literal ; video.mp4"), 1234, arena,
                                 S("failed to probe media"), &log) == 0);
  TEST_CHECK(frame.is_created);
  TEST_CHECK(strncmp(frame.path, temp_dir, strlen(temp_dir)) == 0);
  TEST_CHECK(strstr(frame.path, "XXXXXX") == NULL);
  TEST_CHECK(os_stat(os_posix(), str_from_cstr(frame.path), &st) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(st.len == S("jpeg-frame").len);
  video_frame_remove(&frame);
  TEST_CHECK(!frame.is_created);
  TEST_CHECK(access(frame.path, F_OK) != 0);

  restore_env("PATH", path_saved);
  restore_env("TMPDIR", tmpdir_saved);
  restore_env("FRAM_TEST_LOG", log_saved);
  restore_env("FRAM_TEST_FRAME_MODE", mode_saved);
  remove_fixture_tree(root_dir);
}

// A failed FFmpeg invocation unlinks the already-created temporary and leaves the slot unmarked.
// The fake tool logs the destination so the test can assert its cleanup directly.
static void test_extract_frame_cleans_failed_temp(void) {
  char root_dir[] = "/tmp/fram-video-fail.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  if (write_fake_tools(root_dir, &arena) != 0) {
    remove_fixture_tree(root_dir);
    return;
  }
  char* log_path = fixture_cstr(root_dir, "frame.log", &arena);

  const char* path_saved = save_env("PATH", &arena);
  const char* tmpdir_saved = save_env("TMPDIR", &arena);
  const char* log_saved = save_env("FRAM_TEST_LOG", &arena);
  const char* mode_saved = save_env("FRAM_TEST_FRAME_MODE", &arena);
  TEST_CHECK(setenv("PATH", root_dir, 1) == 0);
  TEST_CHECK(setenv("TMPDIR", root_dir, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_LOG", log_path, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_FRAME_MODE", "fail", 1) == 0);

  struct VideoFrame frame;
  video_frame_init(&frame, &arena);
  struct Log log = test_log();
  TEST_CHECK(video_extract_frame(&frame, S("video.mp4"), 0, arena, S("failed to probe media"),
                                 &log) == -1);
  TEST_CHECK(!frame.is_created);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: process exited with status 3: fake ffmpeg failure "
                         "('video.mp4')\n"));

  struct Str logged_path = {0};
  struct Log read_log = test_log();
  TEST_CHECK(os_read(os_posix(), str_from_cstr(log_path), TEST_FILE_LEN_MAX, &arena, &logged_path,
                     S("failed to read fixture"), &read_log) == 0);
  TEST_CHECK(logged_path.len > 0);
  TEST_CHECK(access(str_cstr(logged_path, &arena), F_OK) != 0);

  restore_env("PATH", path_saved);
  restore_env("TMPDIR", tmpdir_saved);
  restore_env("FRAM_TEST_LOG", log_saved);
  restore_env("FRAM_TEST_FRAME_MODE", mode_saved);
  remove_fixture_tree(root_dir);
}

// An `ffmpeg` that exits successfully but leaves no frame, a directory in its place or an empty
// file fails the extraction, naming the frame and the video it is for. A missing or empty frame is
// removed, and a directory, which `unlink` cannot remove, leaves the slot marked.
static void test_extract_frame_rejects_unusable_frame(void) {
  char root_dir[] = "/tmp/fram-video-unusable.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  if (write_fake_tools(root_dir, &arena) != 0) {
    remove_fixture_tree(root_dir);
    return;
  }
  char* log_path = fixture_cstr(root_dir, "frame.log", &arena);

  const char* path_saved = save_env("PATH", &arena);
  const char* tmpdir_saved = save_env("TMPDIR", &arena);
  const char* log_saved = save_env("FRAM_TEST_LOG", &arena);
  const char* mode_saved = save_env("FRAM_TEST_FRAME_MODE", &arena);
  TEST_CHECK(setenv("PATH", root_dir, 1) == 0);
  TEST_CHECK(setenv("TMPDIR", root_dir, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_LOG", log_path, 1) == 0);

  TEST_CHECK(setenv("FRAM_TEST_FRAME_MODE", "missing", 1) == 0);
  struct VideoFrame frame;
  video_frame_init(&frame, &arena);
  struct Log log = test_log();
  TEST_CHECK(video_extract_frame(&frame, S("video.mp4"), 0, arena, S("failed to probe media"),
                                 &log) == -1);
  TEST_CHECK(!frame.is_created);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: cannot inspect video frame: %s ('%s') "
                         "(for 'video.mp4')\n",
                         strerror(ENOENT), frame.path));

  TEST_CHECK(setenv("FRAM_TEST_FRAME_MODE", "dir", 1) == 0);
  video_frame_init(&frame, &arena);
  log = test_log();
  TEST_CHECK(video_extract_frame(&frame, S("video.mp4"), 0, arena, S("failed to probe media"),
                                 &log) == -1);
  TEST_CHECK(frame.is_created);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: video frame is not a regular file ('%s') "
                         "(for 'video.mp4')\n",
                         frame.path));

  TEST_CHECK(setenv("FRAM_TEST_FRAME_MODE", "empty", 1) == 0);
  video_frame_init(&frame, &arena);
  log = test_log();
  TEST_CHECK(video_extract_frame(&frame, S("video.mp4"), 0, arena, S("failed to probe media"),
                                 &log) == -1);
  TEST_CHECK(!frame.is_created);
  TEST_CHECK(access(frame.path, F_OK) != 0);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: FFmpeg produced an empty video frame ('%s') "
                         "(for 'video.mp4')\n",
                         frame.path));

  restore_env("PATH", path_saved);
  restore_env("TMPDIR", tmpdir_saved);
  restore_env("FRAM_TEST_LOG", log_saved);
  restore_env("FRAM_TEST_FRAME_MODE", mode_saved);
  remove_fixture_tree(root_dir);
}

static char* fixture_cstr(const char* root_dir, const char* relative_path, struct Arena* arena) {
  return str_cstr(path_join(str_from_cstr(root_dir), str_from_cstr(relative_path), arena), arena);
}

static int write_fake_tools(const char* root_dir, struct Arena* arena) {
  static const struct Str probe_script = STR_INIT(
      "#!/bin/sh\n"
      "case \"$FRAM_TEST_PROBE_MODE\" in\n"
      "  long) printf '12.345999\\n' ;;\n"
      "  short) printf '0.4\\n' ;;\n"
      "  unknown) printf '  N/A  \\n' ;;\n"
      "  invalid) printf 'not-a-duration\\n' ;;\n"
      "  nul) printf '12.5\\000junk\\n' ;;\n"
      "  output) printf '%s\\n' \"$FRAM_TEST_PROBE_OUTPUT\" ;;\n"
      "  *) printf 'probe failed\\n' >&2; exit 2 ;;\n"
      "esac\n");
  // `PATH` holds only the test's directory while the fakes run, so the frame script names `rm` and
  // `mkdir` by their absolute paths.
  static const struct Str frame_script = STR_INIT(
      "#!/bin/sh\n"
      "last=\n"
      "for arg do last=$arg; done\n"
      "printf '%s' \"$last\" > \"$FRAM_TEST_LOG\"\n"
      "case \"$FRAM_TEST_FRAME_MODE\" in\n"
      "  fail) printf 'fake ffmpeg failure\\n' >&2; exit 3 ;;\n"
      "  missing) /bin/rm -f \"$last\"; exit 0 ;;\n"
      "  dir) /bin/rm -f \"$last\"; /bin/mkdir \"$last\"; exit 0 ;;\n"
      "  empty) exit 0 ;;\n"
      "esac\n"
      "printf 'jpeg-frame' > \"$last\"\n");
  char* probe_path = fixture_cstr(root_dir, "ffprobe", arena);
  char* frame_path = fixture_cstr(root_dir, "ffmpeg", arena);
  struct Log log = test_log();
  if (os_write(os_posix(), str_from_cstr(probe_path), (struct OsSource){.bytes = probe_script},
               S("failed to write fixture"), &log) != 0 ||
      os_write(os_posix(), str_from_cstr(frame_path), (struct OsSource){.bytes = frame_script},
               S("failed to write fixture"), &log) != 0 ||
      chmod(probe_path, 0700) != 0 || chmod(frame_path, 0700) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  return 0;
}

TEST_LIST = {
    {"frame remove skips uncreated slot", test_frame_remove_skips_uncreated_slot},
    {"frame remove skips removed frame", test_frame_remove_skips_removed_frame},
    {"frame remove clears missing frame", test_frame_remove_clears_missing_frame},
    {"frame remove retries failed unlink", test_frame_remove_retries_failed_unlink},
    {"probe duration parses milliseconds", test_probe_duration_parses_milliseconds},
    {"probe duration rejects overflow", test_probe_duration_rejects_overflow},
    {"probe duration rejects output with nul", test_probe_duration_rejects_output_with_nul},
    {"video rejects path with nul", test_video_rejects_path_with_nul},
    {"extract frame fills slot temp", test_extract_frame_fills_slot_temp},
    {"extract frame cleans failed temp", test_extract_frame_cleans_failed_temp},
    {"extract frame rejects unusable frame", test_extract_frame_rejects_unusable_frame},
    {NULL, NULL},
};
