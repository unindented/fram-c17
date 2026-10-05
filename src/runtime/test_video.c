#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/path.h"
#include "core/text.h"
#include "runtime/fs.h"
#include "runtime/video.h"
#include "shared/arena.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few hundred bytes. */
enum { TEST_FILE_LEN_MAX = 1024 * 1024 };

/** Writes executable fake FFmpeg tools into one test directory. */
static int write_fake_tools(const char* root_dir, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

/** Restores one environment variable from a malloc-owned saved value. */
static void restore_env(const char* name, char* saved) __attribute__((nonnull(1)));

// Decimal seconds from ffprobe are parsed without locale dependence, padded or truncated to three
// fractional digits, and published only after a valid complete value.
static void test_probe_duration_parses_milliseconds(void) {
  char root_dir_template[] = "/tmp/fram-video-probe.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena arena;
  arena_init(&arena);
  if (write_fake_tools(root_dir, &arena) != 0) {
    arena_free(&arena);
    remove_fixture_tree(root_dir);
    return;
  }

  const char* old_path_value = getenv("PATH");
  char* old_path = old_path_value == NULL ? NULL : text_strdup(old_path_value);
  const char* old_mode_value = getenv("FRAM_TEST_PROBE_MODE");
  char* old_mode = old_mode_value == NULL ? NULL : text_strdup(old_mode_value);
  TEST_CHECK(setenv("PATH", root_dir, 1) == 0);

  size_t duration_ms = 99;
  TEST_CHECK(setenv("FRAM_TEST_PROBE_MODE", "long", 1) == 0);
  TEST_CHECK(video_probe_duration("name with ; metacharacters.mp4", &duration_ms, NULL, 0) == 0);
  TEST_CHECK(duration_ms == 12345);

  TEST_CHECK(setenv("FRAM_TEST_PROBE_MODE", "short", 1) == 0);
  TEST_CHECK(video_probe_duration("video.mp4", &duration_ms, NULL, 0) == 0);
  TEST_CHECK(duration_ms == 400);

  TEST_CHECK(setenv("FRAM_TEST_PROBE_MODE", "unknown", 1) == 0);
  duration_ms = 77;
  TEST_CHECK(video_probe_duration("video.mp4", &duration_ms, NULL, 0) == 0);
  TEST_CHECK(duration_ms == 0);

  TEST_CHECK(setenv("FRAM_TEST_PROBE_MODE", "invalid", 1) == 0);
  duration_ms = 77;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(video_probe_duration("video.mp4", &duration_ms, reason, sizeof(reason)) == -1);
  TEST_CHECK(duration_ms == 77);
  TEST_CHECK(strstr(reason, "invalid duration") != NULL);

  restore_env("PATH", old_path);
  restore_env("FRAM_TEST_PROBE_MODE", old_mode);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Frame extraction passes a literal input path, formats the seek timestamp, produces a nonempty
// secure temporary in `TMPDIR`, and transfers ownership of that path to the caller.
static void test_extract_frame_returns_owned_temp(void) {
  char root_dir_template[] = "/tmp/fram-video-frame-test.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena arena;
  arena_init(&arena);
  if (write_fake_tools(root_dir, &arena) != 0) {
    arena_free(&arena);
    remove_fixture_tree(root_dir);
    return;
  }
  char* temp_dir = path_join(root_dir, "tmp", &arena);
  char* log_path = path_join(root_dir, "frame.log", &arena);
  TEST_CHECK(fs_mkdir_p(temp_dir, NULL, 0) == 0);

  const char* old_path_value = getenv("PATH");
  char* old_path = old_path_value == NULL ? NULL : text_strdup(old_path_value);
  const char* old_tmp_value = getenv("TMPDIR");
  char* old_tmp = old_tmp_value == NULL ? NULL : text_strdup(old_tmp_value);
  const char* old_log_value = getenv("FRAM_TEST_LOG");
  char* old_log = old_log_value == NULL ? NULL : text_strdup(old_log_value);
  const char* old_mode_value = getenv("FRAM_TEST_FRAME_MODE");
  char* old_mode = old_mode_value == NULL ? NULL : text_strdup(old_mode_value);
  TEST_CHECK(setenv("PATH", root_dir, 1) == 0);
  TEST_CHECK(setenv("TMPDIR", temp_dir, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_LOG", log_path, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_FRAME_MODE", "success", 1) == 0);

  char* frame_path = NULL;
  struct FsMeta meta;
  TEST_CHECK(video_extract_frame("literal ; video.mp4", 1234, &frame_path, NULL, 0) == 0);
  if (!TEST_CHECK(frame_path != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strncmp(frame_path, temp_dir, strlen(temp_dir)) == 0);
  TEST_CHECK(fs_stat_meta(frame_path, &meta, NULL, 0) == 0);
  TEST_CHECK(meta.size_len == strlen("jpeg-frame"));

cleanup:
  free(frame_path);
  restore_env("PATH", old_path);
  restore_env("TMPDIR", old_tmp);
  restore_env("FRAM_TEST_LOG", old_log);
  restore_env("FRAM_TEST_FRAME_MODE", old_mode);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A failed FFmpeg invocation unlinks the already-created temporary and leaves the output pointer
// untouched. The fake tool logs the destination so the test can assert its cleanup directly.
static void test_extract_frame_cleans_failed_temp(void) {
  char root_dir_template[] = "/tmp/fram-video-fail.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena arena;
  arena_init(&arena);
  if (write_fake_tools(root_dir, &arena) != 0) {
    arena_free(&arena);
    remove_fixture_tree(root_dir);
    return;
  }
  char* log_path = path_join(root_dir, "frame.log", &arena);

  const char* old_path_value = getenv("PATH");
  char* old_path = old_path_value == NULL ? NULL : text_strdup(old_path_value);
  const char* old_tmp_value = getenv("TMPDIR");
  char* old_tmp = old_tmp_value == NULL ? NULL : text_strdup(old_tmp_value);
  const char* old_log_value = getenv("FRAM_TEST_LOG");
  char* old_log = old_log_value == NULL ? NULL : text_strdup(old_log_value);
  const char* old_mode_value = getenv("FRAM_TEST_FRAME_MODE");
  char* old_mode = old_mode_value == NULL ? NULL : text_strdup(old_mode_value);
  TEST_CHECK(setenv("PATH", root_dir, 1) == 0);
  TEST_CHECK(setenv("TMPDIR", root_dir, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_LOG", log_path, 1) == 0);
  TEST_CHECK(setenv("FRAM_TEST_FRAME_MODE", "fail", 1) == 0);

  char sentinel[] = "unchanged";
  char* frame_path = sentinel;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(video_extract_frame("video.mp4", 0, &frame_path, reason, sizeof(reason)) == -1);
  TEST_CHECK(frame_path == sentinel);
  TEST_CHECK(strstr(reason, "fake ffmpeg failure") != NULL);

  unsigned char* logged_path = NULL;
  size_t logged_len = 0;
  TEST_CHECK(fs_read_file(log_path, TEST_FILE_LEN_MAX, &logged_path, &logged_len, NULL, 0) == 0);
  TEST_CHECK(logged_len > 0);
  TEST_CHECK(access((const char*)logged_path, F_OK) != 0);
  free(logged_path);

  restore_env("PATH", old_path);
  restore_env("TMPDIR", old_tmp);
  restore_env("FRAM_TEST_LOG", old_log);
  restore_env("FRAM_TEST_FRAME_MODE", old_mode);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

static int write_fake_tools(const char* root_dir, struct Arena* arena) {
  static const char probe_script[] =
      "#!/bin/sh\n"
      "case \"$FRAM_TEST_PROBE_MODE\" in\n"
      "  long) printf '12.345999\\n' ;;\n"
      "  short) printf '0.4\\n' ;;\n"
      "  unknown) printf '  N/A  \\n' ;;\n"
      "  invalid) printf 'not-a-duration\\n' ;;\n"
      "  *) printf 'probe failed\\n' >&2; exit 2 ;;\n"
      "esac\n";
  static const char frame_script[] =
      "#!/bin/sh\n"
      "last=\n"
      "for arg do last=$arg; done\n"
      "printf '%s' \"$last\" > \"$FRAM_TEST_LOG\"\n"
      "if [ \"$FRAM_TEST_FRAME_MODE\" = fail ]; then\n"
      "  printf 'fake ffmpeg failure\\n' >&2\n"
      "  exit 3\n"
      "fi\n"
      "printf 'jpeg-frame' > \"$last\"\n";
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

static void restore_env(const char* name, char* saved) {
  if (saved == NULL) {
    (void)unsetenv(name);
  } else {
    (void)setenv(name, saved, 1);
    free(saved);
  }
}

TEST_LIST = {
    {"probe duration parses milliseconds", test_probe_duration_parses_milliseconds},
    {"extract frame returns owned temp", test_extract_frame_returns_owned_temp},
    {"extract frame cleans failed temp", test_extract_frame_cleans_failed_temp},
    {NULL, NULL},
};
