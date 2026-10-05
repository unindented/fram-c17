#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app/cli_dispatch.h"
#include "app/exit_code.h"
#include "core/error.h"
#include "core/fram_version.h"
#include "core/path.h"
#include "runtime/fs.h"
#include "shared/arena.h"
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
      rc = cli_dispatch(argc, argv);
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
 * @brief Creates and enters a minimal buildable gallery fixture.
 *
 * The media directory is present but empty, so the build runs without encoding an image and the
 * album page render is its only parallel phase.
 *
 * Cleans partial setup before returning a failure.
 *
 * @param root_dir         Writable `mkdtemp` template. Receives the fixture root.
 * @param saved_dir_fd_out Receives the saved working directory for `leave_gallery_fixture`.
 * @return `root_dir` on success, or `NULL` after recording a test-plumbing failure.
 */
static const char* enter_gallery_fixture(char root_dir[static 1], int* saved_dir_fd_out) {
  const char* created_root_dir = init_fixture_dir(root_dir);
  if (created_root_dir == NULL) {
    return NULL;
  }

  static const char config[] =
      "title = \"Gallery\"\n"
      "author = \"Author\"\n";
  static const char album_template[] = "<title>{{album.title}}</title>\n";
  struct Arena arena;
  arena_init(&arena);
  const char* media_dir = path_join(created_root_dir, "media", &arena);
  const bool written =
      media_dir != NULL && write_fixture_file(created_root_dir, "fram.toml", config) == 0 &&
      write_fixture_file(created_root_dir, "templates/album.html", album_template) == 0 &&
      fs_mkdir_p(media_dir, NULL, 0) == 0;
  arena_free(&arena);
  TEST_CHECK(written);
  if (!written || working_dir_enter(created_root_dir, saved_dir_fd_out) != 0) {
    remove_fixture_tree(created_root_dir);
    return NULL;
  }
  return created_root_dir;
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
  char expected[64];
  const int expected_len = snprintf(expected, sizeof(expected), "fram %s\n", fram_version_string());
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
  char root_dir_template[] = "/tmp/fram-dispatch-gallery.XXXXXX";
  int saved_dir_fd = -1;
  const char* root_dir = enter_gallery_fixture(root_dir_template, &saved_dir_fd);
  if (root_dir == NULL) {
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
                    "assets_dir = \"assets\"\n"
                    "album_template = \"album.html\"\n"
                    "aggregate_templates = []\n"
                    "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
                    "m = { width = 520, height = 360, quality = 80, crop = true }, "
                    "l = { width = 792, height = 594, quality = 85 } }\n"
                    "video_frame_seconds = 1\n") == 0);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
  // No page was written, which is what separates `config` from `build`.
  TEST_CHECK(access("public/index.html", F_OK) != 0);

  leave_gallery_fixture(root_dir, saved_dir_fd);
}

// The `build` command is dispatched to `cmd_build_run` with the parsed options attached, so the
// worker count and the verbose flag reach the build rather than being dropped on the way. The
// verbose phase line naming the requested count is the only externally visible evidence that both
// fields were carried across.
static void test_build_command_receives_parsed_options(void) {
  char root_dir_template[] = "/tmp/fram-dispatch-gallery.XXXXXX";
  int saved_dir_fd = -1;
  const char* root_dir = enter_gallery_fixture(root_dir_template, &saved_dir_fd);
  if (root_dir == NULL) {
    return;
  }

  char* argv[] = {"fram", "build", "--verbose", "--workers=3", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(4, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(dispatch_out.stdout_out[0] == '\0');
  TEST_CHECK(strcmp(dispatch_out.stderr_out,
                    "loading config\n"
                    "discovering media\n"
                    "discovering assets\n"
                    "planning albums\n"
                    "building output manifest\n"
                    "rendering albums, workers: 3\n"
                    "\rrendering albums 1/1\n"
                    "writing album pages\n"
                    "rendering aggregate templates\n"
                    "copying assets\n"
                    "build complete\n") == 0);
  TEST_CHECK(access("public/index.html", F_OK) == 0);

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

// A `stdout` that cannot be written turns a successful print into `EXIT_CODE_FAILURE` and names
// both the subject and the stream's own reason. Nothing else in the suite reaches this arm. Other
// tests make a printer fail, but none goes through `cli_dispatch`, so nothing else pins the mapping
// from a printer's non-zero result to an exit code. A read-only descriptor is the deterministic way
// there, since the write fails inside the printer's `fflush` rather than at the `fprintf`.
static void test_version_write_failure_reports_failure(void) {
  char* argv[] = {"fram", "--version", NULL};
  enum ExitCode rc = (enum ExitCode)TEST_PLUMBING_FAILED;
  bool has_plumbing_failed = true;
  char stderr_out[4096] = "";
  struct StreamCapture stdout_capture;
  struct StreamCapture stderr_capture;
  if (capture_begin_unwritable(stdout, &stdout_capture) == 0) {
    if (capture_begin(stderr, &stderr_capture) == 0) {
      rc = cli_dispatch(2, argv);
      has_plumbing_failed = capture_end(&stderr_capture, stderr_out, sizeof(stderr_out)) != 0;
    }
    has_plumbing_failed = capture_end(&stdout_capture, NULL, 0) != 0 || has_plumbing_failed;
  }

  TEST_CHECK(!has_plumbing_failed);
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  char reason[ERROR_MESSAGE_SIZE];
  char expected[ERROR_MESSAGE_SIZE * 2];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to write version to 'stdout': %s\n",
               error_system_message(reason, sizeof(reason), EBADF));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);
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
