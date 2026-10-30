// `setenv` is POSIX rather than C17, so this file asks for it.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app/cmd_build.h"
#include "build/job.h"
#include "core/path.h"
#include "runtime/fs.h"
#include "runtime/os.h"
#include "runtime/pool.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_jpeg_fixture.h"
#include "test_support.h"

// The builds of galleries that hold videos. They run `ffprobe` and `ffmpeg` fakes on `PATH`, and a
// video's frame is a file on the real host, so these run against the POSIX host in a real fixture
// directory. Every other build test is in `test_cmd_build.c`, on the in-memory host.

/** Config body every fixture shares. Derivatives stay small so encoding stays cheap. */
static const char* const GALLERY_CONFIG =
    "title = \"Test Gallery\"\n"
    "author = \"Tests\"\n"
    "input_dir = \"photos\"\n"
    "[derivatives.s]\nwidth = 8\nheight = 8\nquality = 70\ncrop = true\n"
    "[derivatives.m]\nwidth = 16\nheight = 16\nquality = 75\n";

/** Minimal album template. It renders without naming any derivative, so sizes stay free. */
static const char* const ALBUM_TEMPLATE =
    "<!doctype html><title>{{album.title}} - {{gallery.title}}</title>"
    "{{#has_media}}<ul>{{#media}}<li>{{title}}</li>{{/media}}</ul>{{/has_media}}";

/**
 * @brief Writes `fram.toml` and the album template below a fixture root.
 *
 * @param root_dir Fixture root directory. Must not be `NULL`.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
static int write_gallery_fixture(const char* root_dir) {
  if (write_fixture_file(root_dir, "templates/album.html", ALBUM_TEMPLATE) != 0) {
    return -1;
  }
  return write_fixture_file(root_dir, "fram.toml", GALLERY_CONFIG);
}

/**
 * @brief Writes one script as both a fake `ffprobe` and a fake `ffmpeg` in `bin/` below a fixture
 *        root, executable by the owner.
 *
 * @param root_dir Fixture root directory. Must not be `NULL`.
 * @param script   Script text both tools run.
 * @param arena    Arena for the terminated script and the tool paths. Must not be `NULL`.
 * @return `0` on success, or `-1` after failing the current test check.
 */
static int write_fake_video_tools(const char* root_dir, struct Str script, struct Arena* arena) {
  const char* script_text = str_cstr(script, arena);
  const struct Str bin_dir = path_join(str_from_cstr(root_dir), S("bin"), arena);
  const char* ffprobe_path = str_cstr(path_join(bin_dir, S("ffprobe"), arena), arena);
  const char* ffmpeg_path = str_cstr(path_join(bin_dir, S("ffmpeg"), arena), arena);
  if (!TEST_CHECK(write_fixture_file(root_dir, "bin/ffprobe", script_text) == 0) ||
      !TEST_CHECK(write_fixture_file(root_dir, "bin/ffmpeg", script_text) == 0) ||
      !TEST_CHECK(chmod(ffprobe_path, 0700) == 0) || !TEST_CHECK(chmod(ffmpeg_path, 0700) == 0)) {
    return -1;
  }
  return 0;
}

/**
 * @brief Executes a build in a fixture directory and collects its errors.
 *
 * The build resolves `fram.toml` from the working directory, so this changes into the fixture and
 * restores the previous directory before returning.
 *
 * @param options  Build options passed to `cmd_build_execute`, whose worker count `0` stands for
 *                 the CPU count, as on the command line. Must not be `NULL`.
 * @param root_dir Fixture directory in which to execute the build. Must not be `NULL`.
 * @param log      Log that receives every build error. Its arena must not be one the build
 *                 allocates from, as `cmd_build_execute` requires. Must not be `NULL`.
 * @return `0` on success, `-1` on build failure, or `TEST_PLUMBING_FAILED` on plumbing failure.
 */
static int execute_build_in(const struct BuildOptions* options,
                            const char* root_dir,
                            struct Log* log) {
  int saved_dir_fd = -1;
  if (working_dir_enter(root_dir, &saved_dir_fd) != 0) {
    return TEST_PLUMBING_FAILED;
  }
  struct BuildOptions resolved = *options;
  if (resolved.worker_count == 0) {
    resolved.worker_count = pool_resolve_worker_count();
  }
  struct Arena perm = test_arena();
  struct JobArenas* worker_arenas = arena_new(&perm, resolved.worker_count, struct JobArenas);
  for (ptrdiff_t i = 0; i < resolved.worker_count; i++) {
    worker_arenas[i] = (struct JobArenas){.perm = test_arena(), .scratch = test_arena()};
  }
  const int rc = cmd_build_execute(&resolved, worker_arenas, os_posix(), perm, test_arena(), log);
  return working_dir_leave(saved_dir_fd) == 0 ? rc : TEST_PLUMBING_FAILED;
}

/**
 * @brief Builds a fixture with one worker and returns which fake tools ran, one name per line, then
 *        empties the record for the next build.
 *
 * @param root_dir Fixture directory holding the gallery and the fake tools. Must not be `NULL`.
 * @param log_path Terminated file the fake tools append their names to. Must not be `NULL`.
 * @param log      Log that receives the build's errors. Must not be `NULL`.
 * @param arena    Arena that owns the returned record. Must not be `NULL`.
 * @return The names of the tools the build ran, in order, or `NULL` when the build failed.
 */
static const char* build_and_take_tool_runs(const char* root_dir,
                                            const char* log_path,
                                            struct Log* log,
                                            struct Arena* arena) {
  const struct BuildOptions options = {.worker_count = 1};
  if (!TEST_CHECK(execute_build_in(&options, root_dir, log) == 0)) {
    return NULL;
  }
  struct Str runs = {0};
  struct Log read_log = test_log();
  if (os_read(os_posix(), str_from_cstr(log_path), 4096, arena, &runs, S("failed to read runs"),
              &read_log) != 0) {
    runs = S("");
  }
  struct Log write_log = test_log();
  TEST_CHECK(os_write(os_posix(), str_from_cstr(log_path), (struct OsSource){.bytes = S("")},
                      S("failed to empty runs"), &write_log) == 0);
  return str_cstr(runs, arena);
}

// A video the build cache answers for runs neither `ffprobe` nor `ffmpeg` while its derivatives are
// fresh. Once one is stale, the build extracts the frame for it, so only `ffmpeg` runs, and the
// derivative is written again.
static void test_cached_video_extracts_its_frame_only_when_needed(void) {
  char root_dir[] = "/tmp/fram-build-cached-video.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  static const char script[] =
      "#!/bin/sh\n"
      "[ \"$1\" = -version ] && exit 0\n"
      "printf '%s\\n' \"${0##*/}\" >> \"$FRAM_TEST_LOG\"\n"
      "case \"$0\" in\n"
      "  *ffprobe) printf '1.000\\n' ;;\n"
      "  *) for arg do last=$arg; done; /bin/cp \"$FRAM_TEST_FRAME\" \"$last\" ;;\n"
      "esac\n";
  struct Arena arena = test_arena();
  const char* path_saved = save_env("PATH", &arena);
  const char* frame_saved = save_env("FRAM_TEST_FRAME", &arena);
  const char* log_saved = save_env("FRAM_TEST_LOG", &arena);
  const struct Str root = str_from_cstr(root_dir);
  const struct Str bin_dir = path_join(root, S("bin"), &arena);
  const struct Str frame_path = path_join(root, S("frame.jpg"), &arena);
  const char* log_path = str_cstr(path_join(root, S("runs.txt"), &arena), &arena);
  const struct Str derivative_path =
      path_join(root, S("public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/clip-mp4-s.webp"), &arena);
  struct Str frame = {0};
  struct Log log = test_log();
  if (TEST_CHECK(write_gallery_fixture(root_dir) == 0) &&
      write_fake_video_tools(root_dir, str_from_cstr(script), &arena) == 0 &&
      TEST_CHECK(write_fixture_file(root_dir, "photos/clip.mp4", "") == 0) &&
      TEST_CHECK(test_jpeg_encode(24, 12, &arena, &frame) == 0) &&
      TEST_CHECK(os_write(os_posix(), frame_path, (struct OsSource){.bytes = frame},
                          S("failed to write fixture"), &log) == 0) &&
      TEST_CHECK(setenv("PATH", str_cstr(bin_dir, &arena), 1) == 0) &&
      TEST_CHECK(setenv("FRAM_TEST_FRAME", str_cstr(frame_path, &arena), 1) == 0) &&
      TEST_CHECK(setenv("FRAM_TEST_LOG", log_path, 1) == 0)) {
    const char* runs = build_and_take_tool_runs(root_dir, log_path, &log, &arena);
    TEST_CHECK(runs != NULL && strcmp(runs, "ffprobe\nffmpeg\n") == 0);
    runs = build_and_take_tool_runs(root_dir, log_path, &log, &arena);
    TEST_CHECK(runs != NULL && strcmp(runs, "") == 0);
    TEST_CHECK(os_remove(os_posix(), derivative_path, S("failed to remove fixture"), &log) == 0);
    runs = build_and_take_tool_runs(root_dir, log_path, &log, &arena);
    TEST_CHECK(runs != NULL && strcmp(runs, "ffmpeg\n") == 0);
    TEST_MSG("runs: '%s'", runs != NULL ? runs : "");
    struct OsStat st;
    TEST_CHECK(os_stat(os_posix(), derivative_path, &st) == 0 && st.len > 0);
    TEST_CHECK(test_log_is_empty(&log));
  }
  restore_env("PATH", path_saved);
  restore_env("FRAM_TEST_FRAME", frame_saved);
  restore_env("FRAM_TEST_LOG", log_saved);
  remove_fixture_tree(root_dir);
}

// Many videos whose probe child fails with a full standard-error tail are reported normally: the
// first `JOB_ERROR_REPORT_COUNT_MAX` failures whole, each with its tail on one line, then a count
// of the rest. A worker keeps the logs of only those failures, and
// `test_run_keeps_the_logs_of_only_reportable_jobs` in `src/build/test_job.c` shows that a share
// that holds them is enough however many jobs fail.
static void test_reports_many_failed_children(void) {
  // Each line the child writes is 64 bytes with its newline, so the 4 KiB tail keeps the newest
  // `TAIL_LINE_COUNT` of them whole.
  enum { VIDEO_COUNT = 100, STDERR_LINE_COUNT = 128, TAIL_LINE_COUNT = 64 };
  static const char stderr_line[] =
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
  char root_dir[] = "/tmp/fram-build-many-children.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  const struct BuildOptions options = {.worker_count = 1};
  struct Arena arena = test_arena();
  struct Buf script = {.arena = &arena};
  buf_printf(&script,
             "#!/bin/sh\n"
             "[ \"$1\" = -version ] && exit 0\n"
             "i=0\n"
             "while [ $i -lt %d ]; do\n"
             "  printf '%%s\\n' '%s' >&2\n"
             "  i=$((i + 1))\n"
             "done\n"
             "exit 1\n",
             STDERR_LINE_COUNT, stderr_line);
  // The failure shows the tail on one line.
  struct Buf tail = {.arena = &arena};
  for (int i = 0; i < TAIL_LINE_COUNT; i++) {
    buf_printf(&tail, "%s%s", i > 0 ? "; " : "", stderr_line);
  }
  struct Buf expected = {.arena = &arena};
  for (int i = 0; i < JOB_ERROR_REPORT_COUNT_MAX; i++) {
    buf_printf(&expected,
               "failed to probe media: process exited with status 1: ...%s ('photos/v%03d.mp4')\n",
               buf_cstr(&tail), i);
  }
  buf_printf(&expected, "\xE2\x80\xA6 and %d more failures\n",
             VIDEO_COUNT - JOB_ERROR_REPORT_COUNT_MAX);
  struct Log log = test_log();
  const char* path_saved = save_env("PATH", &arena);
  const struct Str bin_dir = path_join(str_from_cstr(root_dir), S("bin"), &arena);
  if (!TEST_CHECK(write_gallery_fixture(root_dir) == 0) ||
      write_fake_video_tools(root_dir, buf_str(&script), &arena) != 0) {
    goto cleanup;
  }
  for (int i = 0; i < VIDEO_COUNT; i++) {
    char relative[64];
    (void)snprintf(relative, sizeof(relative), "photos/v%03d.mp4", i);
    if (!TEST_CHECK(write_fixture_file(root_dir, relative, "") == 0)) {
      goto cleanup;
    }
  }
  if (!TEST_CHECK(setenv("PATH", str_cstr(bin_dir, &arena), 1) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(execute_build_in(&options, root_dir, &log) == -1);
  TEST_CHECK(test_log_is(&log, "%s", buf_cstr(&expected)));
  TEST_CHECK(log.errors == VIDEO_COUNT);

cleanup:
  restore_env("PATH", path_saved);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"cached video extracts its frame only when needed",
     test_cached_video_extracts_its_frame_only_when_needed},
    {"reports many failed children", test_reports_many_failed_children},
    {NULL, NULL},
};
