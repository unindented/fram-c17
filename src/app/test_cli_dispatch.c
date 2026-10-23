#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app/cli_dispatch.h"
#include "app/exit_code.h"
#include "core/fram_version.h"
#include "core/path.h"
#include "runtime/fs.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_jpeg_fixture.h"
#include "test_support.h"

/**
 * Captured output of one `cli_dispatch` call. Both streams are sized for the whole usage text,
 * which is the longest thing any dispatch path writes.
 */
struct DispatchOutput {
  /** Everything written to `stdout`, terminated. */
  char stdout_out[8192];

  /** Everything written to `stderr`, terminated. */
  char stderr_out[4096];
};

/**
 * @brief Runs CLI dispatch while capturing both standard streams.
 *
 * Restores both streams before returning.
 *
 * @param argc         Number of arguments in `argv`.
 * @param argv         Argument vector passed to `cli_dispatch`.
 * @param dispatch_out Captured, terminated `stdout` and `stderr` text.
 * @return The dispatch exit code, or `TEST_PLUMBING_FAILED` on test-plumbing failure.
 */
static enum ExitCode dispatch_capturing(int argc,
                                        char** argv,
                                        struct DispatchOutput* dispatch_out) {
  dispatch_out->stdout_out[0] = '\0';
  dispatch_out->stderr_out[0] = '\0';
  enum ExitCode rc = (enum ExitCode)TEST_PLUMBING_FAILED;
  bool has_plumbing_failed = true;
  struct StreamCapture stdout_capture;
  struct StreamCapture stderr_capture;
  if (capture_begin(stdout, &stdout_capture) == 0) {
    if (capture_begin(stderr, &stderr_capture) == 0) {
      rc = cli_dispatch(argc, argv, os_posix());
      has_plumbing_failed = capture_end(&stderr_capture, dispatch_out->stderr_out,
                                        sizeof(dispatch_out->stderr_out)) != 0;
    }
    has_plumbing_failed = capture_end(&stdout_capture, dispatch_out->stdout_out,
                                      sizeof(dispatch_out->stdout_out)) != 0 ||
                          has_plumbing_failed;
  }
  return has_plumbing_failed ? (enum ExitCode)TEST_PLUMBING_FAILED : rc;
}

/**
 * @brief Creates and enters a minimal buildable gallery fixture with two photos.
 *
 * Both photos are in the root album, so the probe and derivative phases run two jobs and the album
 * page render runs one.
 *
 * Cleans partial setup before returning a failure.
 *
 * @param root_dir         Writable `mkdtemp` template. Receives the fixture root.
 * @param saved_dir_fd_out Receives the saved working directory for `leave_gallery_fixture`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int enter_gallery_fixture(char root_dir[static 1], int* saved_dir_fd_out) {
  if (init_fixture_dir(root_dir) != 0) {
    return -1;
  }

  static const char config[] =
      "title = \"Gallery\"\n"
      "author = \"Author\"\n";
  static const char album_template[] = "<title>{{album.title}}</title>\n";
  struct Arena arena = test_arena();
  struct Str jpeg = {0};
  struct Log log = test_log();
  bool written = write_fixture_file(root_dir, "fram.toml", config) == 0 &&
                 write_fixture_file(root_dir, "templates/album.html", album_template) == 0 &&
                 test_jpeg_encode(24, 12, &arena, &jpeg) == 0;
  static const char* const photo_paths[] = {"media/a.jpg", "media/b.jpg"};
  for (ptrdiff_t i = 0; written && i < (ptrdiff_t)(sizeof(photo_paths) / sizeof(photo_paths[0]));
       i++) {
    const struct Str photo_path =
        path_join(str_from_cstr(root_dir), str_from_cstr(photo_paths[i]), &arena);
    written = os_write(os_posix(), photo_path, (struct OsSource){.bytes = jpeg},
                       S("failed to set up fixture"), &log) == 0;
  }
  TEST_CHECK(written);
  if (!written || working_dir_enter(root_dir, saved_dir_fd_out) != 0) {
    remove_fixture_tree(root_dir);
    return -1;
  }
  return 0;
}

/**
 * @brief Restores the working directory and removes a gallery fixture.
 *
 * @param root_dir     Fixture root directory to remove.
 * @param saved_dir_fd Working directory `enter_gallery_fixture` saved.
 */
static void leave_gallery_fixture(const char* root_dir, int saved_dir_fd) {
  (void)working_dir_leave(saved_dir_fd);
  remove_fixture_tree(root_dir);
}

// `--version` writes the version to `stdout` and reports success.
static void test_version_action_reports_ok(void) {
  char* argv[] = {"fram", "--version", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_OK);
  const struct Str version = fram_version_string();
  char expected[64];
  const int expected_len = snprintf(expected, sizeof(expected), "fram %.*s\n", STR_FMT(version));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(dispatch_out.stdout_out, expected) == 0);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
}

// `--help` writes the usage text to `stdout` and reports success. The program name comes from
// `argv[0]`. Using a name that is deliberately not `"fram"` proves the function substitutes it
// rather than hard-coding it. `test_cli.c` pins the text itself, so this test asserts only the
// plumbing.
static void test_help_action_reports_ok(void) {
  char* argv[] = {"/opt/bin/myfram", "--help", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(strstr(dispatch_out.stdout_out, "/opt/bin/myfram") != NULL);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
}

// The `config` command is dispatched to `cmd_config_run`, not to a build: the resolved
// configuration lands on `stdout` and no `public/` directory appears. Dispatching it to the wrong
// command would otherwise still exit `0` in a valid gallery.
static void test_config_command_is_dispatched(void) {
  char root_dir[] = "/tmp/fram-dispatch-gallery.XXXXXX";
  int saved_dir_fd = -1;
  if (enter_gallery_fixture(root_dir, &saved_dir_fd) != 0) {
    return;
  }

  char* argv[] = {"fram", "config", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(strcmp(dispatch_out.stdout_out,
                    "title = \"Gallery\"\n"
                    "author = \"Author\"\n"
                    "base_url = \"\"\n"
                    "input_dir = \"media\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "static_dir = \"static\"\n"
                    "album_template = \"album.html\"\n"
                    "aggregate_templates = []\n"
                    "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
                    "m = { width = 520, height = 360, quality = 80, crop = true }, "
                    "l = { width = 792, height = 594, quality = 85 } }\n"
                    "sort = { albums = \"asc\", media = \"asc\" }\n"
                    "video_frame_seconds = 1\n") == 0);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
  // No page was written, which is what separates `config` from `build`.
  TEST_CHECK(access("public/index.html", F_OK) != 0);

  leave_gallery_fixture(root_dir, saved_dir_fd);
}

// The `build` command is dispatched to `cmd_build_run` with the parsed options attached, so the
// worker count and the verbose flag reach the build rather than being dropped on the way. The
// verbose phase lines naming the requested count are the only externally visible evidence that both
// fields were carried across. The test asks for one worker, so a dropped count would show the
// detected count, on a host with more than one core. One worker also finishes the jobs in order, so
// the progress lines can be compared.
static void test_build_command_receives_parsed_options(void) {
  char root_dir[] = "/tmp/fram-dispatch-gallery.XXXXXX";
  int saved_dir_fd = -1;
  if (enter_gallery_fixture(root_dir, &saved_dir_fd) != 0) {
    return;
  }

  char* argv[] = {"fram", "build", "--verbose", "--workers=1", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(4, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(dispatch_out.stdout_out[0] == '\0');
  TEST_CHECK(strcmp(dispatch_out.stderr_out,
                    "loading config\n"
                    "discovering media\n"
                    "discovering static files\n"
                    "planning albums\n"
                    "probing media, workers: 1\n"
                    "\rprobing media 1/2\rprobing media 2/2\n"
                    "building output manifest\n"
                    "generating derivatives, workers: 1\n"
                    "\rgenerating derivatives 1/2\rgenerating derivatives 2/2\n"
                    "rendering albums, workers: 1\n"
                    "\rrendering albums 1/1\n"
                    "writing album pages\n"
                    "rendering aggregate templates\n"
                    "copying static files\n"
                    "build complete\n") == 0);
  TEST_CHECK(access("public/index.html", F_OK) == 0);
  TEST_CHECK(access("public/_fram/originals/a-jpg.jpg", F_OK) == 0);
  TEST_CHECK(access("public/_fram/originals/b-jpg.jpg", F_OK) == 0);

  leave_gallery_fixture(root_dir, saved_dir_fd);
}

// A command line that fails to parse reports `EXIT_CODE_USAGE`, and the parser's own diagnostic
// goes to `stderr` rather than `stdout`. The exit code is what nothing else pins: `2` is what a
// shell or CI wrapper branches on to tell a bad invocation from a failed build, and `test_cli.c`
// asserts only that `cli_parse` produced the message.
static void test_parse_error_reports_usage_code(void) {
  char* argv[] = {"fram", "--bogus", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_USAGE);
  TEST_CHECK(strcmp(dispatch_out.stderr_out, "unknown option '--bogus'\n") == 0);
  TEST_CHECK(dispatch_out.stdout_out[0] == '\0');
}

// A `stdout` that cannot be written turns a successful print into `EXIT_CODE_FAILURE` and prints
// nothing to `stderr`. Nothing else in the suite reaches this arm. Other tests make a printer fail,
// but none goes through `cli_dispatch`, so nothing else pins the mapping from a printer's non-zero
// result to an exit code. A read-only descriptor is the deterministic way there, since every write
// to it fails with `EBADF`.
static void test_version_write_failure_reports_failure(void) {
  char* argv[] = {"fram", "--version", NULL};
  enum ExitCode rc = (enum ExitCode)TEST_PLUMBING_FAILED;
  bool has_plumbing_failed = true;
  char stderr_out[4096] = "";
  struct StreamCapture stdout_capture;
  struct StreamCapture stderr_capture;
  if (capture_begin_unwritable(stdout, &stdout_capture) == 0) {
    if (capture_begin(stderr, &stderr_capture) == 0) {
      rc = cli_dispatch(2, argv, os_posix());
      has_plumbing_failed = capture_end(&stderr_capture, stderr_out, sizeof(stderr_out)) != 0;
    }
    has_plumbing_failed = capture_end(&stdout_capture, NULL, 0) != 0 || has_plumbing_failed;
  }

  TEST_CHECK(!has_plumbing_failed);
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  TEST_CHECK(strcmp(stderr_out, "") == 0);
}

TEST_LIST = {
    {"version action reports ok", test_version_action_reports_ok},
    {"help action reports ok", test_help_action_reports_ok},
    {"config command is dispatched", test_config_command_is_dispatched},
    {"build command receives parsed options", test_build_command_receives_parsed_options},
    {"parse error reports usage code", test_parse_error_reports_usage_code},
    {"version write failure reports failure", test_version_write_failure_reports_failure},
    {NULL, NULL},
};
