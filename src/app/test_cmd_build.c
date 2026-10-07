#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/cmd_build.h"
#include "app/exit_code.h"
#include "build/test_jpeg.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"
#include "test_support.h"

/** Largest generated file a test reads back, in bytes. Fixture outputs are a few kilobytes. */
enum { TEST_FILE_LEN_MAX = 1024 * 1024 };

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
 * @brief Reads one generated file below a fixture root.
 *
 * @param root_dir               Fixture root directory.
 * @param relative_path          Relative generated-file path below `root_dir`.
 * @param generated_file_out     Receives allocated file contents on success.
 * @param generated_file_len_out Receives the content length in bytes on success.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
static int read_fixture_file(const char* root_dir,
                             const char* relative_path,
                             unsigned char** generated_file_out,
                             size_t* generated_file_len_out) {
  struct Arena arena;
  arena_init(&arena);
  char* fixture_path = path_join(root_dir, relative_path, &arena);
  const int rc = fixture_path == NULL
                     ? -1
                     : fs_read_file(fixture_path, TEST_FILE_LEN_MAX, generated_file_out,
                                    generated_file_len_out, NULL, 0);
  arena_free(&arena);
  return rc;
}

/**
 * @brief Writes one JPEG below a fixture root, creating parent directories as needed.
 *
 * The fixture encodes its own source rather than committing a binary file, so no test needs a
 * checked-in image and none needs `ffmpeg`.
 *
 * @param root_dir      Fixture root directory.
 * @param relative_path Relative path below `root_dir` to write.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
static int write_fixture_jpeg(const char* root_dir, const char* relative_path) {
  unsigned char* jpeg = NULL;
  size_t jpeg_len = 0;
  if (test_jpeg_encode(24, 12, &jpeg, &jpeg_len) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  struct Arena arena;
  arena_init(&arena);
  char* path = path_join(root_dir, relative_path, &arena);
  const int rc = path == NULL ? -1 : fs_write_file(path, jpeg, jpeg_len, NULL, 0);
  TEST_CHECK(rc == 0);
  arena_free(&arena);
  free(jpeg);
  return rc;
}

/**
 * @brief Writes `fram.toml` and the album template below a fixture root.
 *
 * @param root_dir     Fixture root directory.
 * @param config_extra Config lines placed before the shared body, or `NULL` for the body alone. It
 *                     must not repeat a key the body sets, because TOML rejects a duplicate key.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
static int write_gallery_fixture(const char* root_dir, const char* config_extra) {
  if (write_fixture_file(root_dir, "templates/album.html", ALBUM_TEMPLATE) != 0) {
    return -1;
  }
  if (config_extra == NULL) {
    return write_fixture_file(root_dir, "fram.toml", GALLERY_CONFIG);
  }
  struct StringBuffer config;
  string_buffer_init(&config);
  int rc = string_buffer_append(&config, config_extra) != 0 ||
                   string_buffer_append(&config, GALLERY_CONFIG) != 0
               ? -1
               : 0;
  if (rc == 0) {
    rc = write_fixture_file(root_dir, "fram.toml", config.data);
  }
  string_buffer_free(&config);
  return rc;
}

/**
 * @brief Executes a build in a fixture directory and collects its diagnostic.
 *
 * The build resolves `fram.toml` from the working directory, so this changes into the fixture and
 * restores the previous directory before returning.
 *
 * @param root_dir  Fixture directory in which to execute the build.
 * @param error_out Buffer that receives any build diagnostic.
 * @return `0` on success, `-1` on build failure, or `TEST_PLUMBING_FAILED` on plumbing failure.
 */
static int execute_build_in_dir(const char* root_dir, struct StringBuffer* error_out) {
  int saved_dir_fd = -1;
  if (working_dir_enter(root_dir, &saved_dir_fd) != 0) {
    return TEST_PLUMBING_FAILED;
  }
  const struct BuildOptions options = {0};
  const int rc = cmd_build_execute(&options, error_out);
  return working_dir_leave(saved_dir_fd) == 0 ? rc : TEST_PLUMBING_FAILED;
}

/**
 * @brief Runs a build while capturing both standard streams.
 *
 * Restores both streams and the working directory before returning.
 *
 * @param root_dir       Fixture directory in which to run the build.
 * @param options        Build options passed to `cmd_build_run`.
 * @param stdout_out     Buffer that receives terminated standard output.
 * @param stdout_out_len Size of `stdout_out` in bytes. Must be non-zero.
 * @param stderr_out     Buffer that receives terminated standard error.
 * @param stderr_out_len Size of `stderr_out` in bytes. Must be non-zero.
 * @return The command exit code, or `TEST_PLUMBING_FAILED` on test-plumbing failure.
 */
static enum ExitCode run_build_capturing(const char* root_dir,
                                         const struct BuildOptions* options,
                                         char* stdout_out,
                                         size_t stdout_out_len,
                                         char* stderr_out,
                                         size_t stderr_out_len) {
  stdout_out[0] = '\0';
  stderr_out[0] = '\0';
  int saved_dir_fd = -1;
  if (working_dir_enter(root_dir, &saved_dir_fd) != 0) {
    return (enum ExitCode)TEST_PLUMBING_FAILED;
  }

  enum ExitCode rc = (enum ExitCode)TEST_PLUMBING_FAILED;
  bool has_plumbing_failed = true;
  struct StreamCapture stdout_capture;
  struct StreamCapture stderr_capture;
  if (capture_begin(stdout, &stdout_capture) == 0) {
    if (capture_begin(stderr, &stderr_capture) == 0) {
      rc = cmd_build_run(options);
      has_plumbing_failed = capture_end(&stderr_capture, stderr_out, stderr_out_len) != 0;
    }
    has_plumbing_failed =
        capture_end(&stdout_capture, stdout_out, stdout_out_len) != 0 || has_plumbing_failed;
  }
  has_plumbing_failed = working_dir_leave(saved_dir_fd) != 0 || has_plumbing_failed;
  return has_plumbing_failed ? (enum ExitCode)TEST_PLUMBING_FAILED : rc;
}

/**
 * @brief Reports whether a path list contains a fixture-relative path.
 *
 * @param outputs       Path list to search.
 * @param root_dir      Fixture root directory.
 * @param relative_path Relative path to join to `root_dir`.
 * @return `true` when the joined path is present, or `false` otherwise.
 */
static bool has_output_path(const struct PathList* outputs,
                            const char* root_dir,
                            const char* relative_path) {
  struct Arena arena;
  arena_init(&arena);
  char* want = path_join(root_dir, relative_path, &arena);
  bool found = false;
  for (size_t i = 0; want != NULL && !found && i < outputs->count; i++) {
    found = strcmp(outputs->items[i], want) == 0;
  }
  arena_free(&arena);
  return found;
}

/**
 * @brief Reports whether a path below a fixture root exists.
 *
 * @param root_dir      Fixture root directory.
 * @param relative_path Relative path below `root_dir` to inspect.
 * @return `true` when the path exists, or `false` otherwise.
 */
static bool fixture_path_exists(const char* root_dir, const char* relative_path) {
  struct Arena arena;
  arena_init(&arena);
  char* path = path_join(root_dir, relative_path, &arena);
  const bool exists = path != NULL && access(path, F_OK) == 0;
  arena_free(&arena);
  return exists;
}

// A configured album template name is used instead of the default.
static void test_honors_configured_album_template(void) {
  char root_dir_template[] = "/tmp/fram-build-album-template.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  unsigned char* generated_file = NULL;
  size_t generated_file_len = 0;
  if (!TEST_CHECK(write_gallery_fixture(root_dir, "album_template = \"album-page.html\"\n") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/one.jpg") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/album-page.html",
                                     "<main>{{album.title}}</main>\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");

  TEST_CHECK(
      read_fixture_file(root_dir, "public/index.html", &generated_file, &generated_file_len) == 0);
  TEST_CHECK(generated_file != NULL &&
             strcmp((const char*)generated_file, "<main>Test Gallery</main>\n") == 0);

cleanup:
  free(generated_file);
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// A build writes exactly the manifest's intended outputs: album pages, derivatives, originals,
// aggregate outputs and static files, and nothing for a file in `input_dir` that is not media.
static void test_writes_exactly_manifest_outputs(void) {
  char root_dir_template[] = "/tmp/fram-build-manifest-outputs.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  static const char* expected[] = {
      "public/_fram/originals/album/one-jpg.jpg",
      "public/_fram/v1-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.jpg",
      "public/_fram/v1-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.jpg",
      "public/album/index.html",
      "public/fram.css",
      "public/index.html",
      "public/sitemap.xml",
  };
  const size_t expected_count = sizeof(expected) / sizeof(expected[0]);
  static const char* const all_suffixes[] = {""};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  struct Arena arena;
  arena_init(&arena);
  struct PathList outputs;
  path_list_init(&outputs);
  char* output_dir = NULL;
  if (!TEST_CHECK(write_gallery_fixture(root_dir, "aggregate_templates = [\"sitemap.xml\"]\n") ==
                  0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/sitemap.xml", "sitemap\n") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "photos/album/notes.txt", "not media\n") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "static/fram.css", "body{}\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");

  output_dir = path_join(root_dir, "public", &arena);
  if (!TEST_CHECK(output_dir != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(fs_list_files_with_suffixes(&outputs, output_dir, NULL, all_suffixes, 1, false, NULL,
                                         0) == 0);

  // Exactly the expected set: matching count plus membership rules out extras and the non-media
  // file.
  TEST_CHECK(outputs.count == expected_count);
  for (size_t i = 0; i < expected_count; i++) {
    TEST_CHECK(has_output_path(&outputs, root_dir, expected[i]));
    TEST_MSG("missing: '%s'", expected[i]);
  }

cleanup:
  path_list_free(&outputs);
  arena_free(&arena);
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// A trailing slash on `input_dir` does not leak the directory name into the output tree. The prefix
// strip that places each media item in its album compares `<input_dir>/`, so an unnormalized
// `"photos/"` would miss and publish `public/photos/album/index.html`.
static void test_tolerates_trailing_slash_on_input_dir(void) {
  char root_dir_template[] = "/tmp/fram-build-trailing-slash.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  // The shared config sets `input_dir`, so this fixture writes its own.
  const char config[] =
      "title = \"Test Gallery\"\n"
      "author = \"Tests\"\n"
      "input_dir = \"photos/\"\n"
      "[derivatives.s]\nwidth = 8\nheight = 8\nquality = 70\ncrop = true\n"
      "[derivatives.m]\nwidth = 16\nheight = 16\nquality = 75\n";
  static const char* const all_suffixes[] = {""};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  unsigned char* generated_file = NULL;
  size_t generated_file_len = 0;
  unsigned char* leaked_file = NULL;
  size_t leaked_file_len = 0;
  struct Arena arena;
  arena_init(&arena);
  struct PathList outputs;
  path_list_init(&outputs);
  char* output_dir = NULL;
  if (!TEST_CHECK(write_fixture_file(root_dir, "fram.toml", config) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/album.html", ALBUM_TEMPLATE) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");

  TEST_CHECK(read_fixture_file(root_dir, "public/album/index.html", &generated_file,
                               &generated_file_len) == 0);
  TEST_CHECK(generated_file_len > 0);

  // The path an unnormalized `input_dir` would publish instead.
  TEST_CHECK(read_fixture_file(root_dir, "public/photos/album/index.html", &leaked_file,
                               &leaked_file_len) == -1);

  // Nor does the name reach a derivative or original path.
  output_dir = path_join(root_dir, "public", &arena);
  if (!TEST_CHECK(output_dir != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(fs_list_files_with_suffixes(&outputs, output_dir, NULL, all_suffixes, 1, false, NULL,
                                         0) == 0);
  TEST_CHECK(outputs.count == 5);
  for (size_t i = 0; i < outputs.count; i++) {
    TEST_CHECK(strstr(outputs.items[i], "/photos/") == NULL);
    TEST_MSG("leaked: '%s'", outputs.items[i]);
  }

cleanup:
  path_list_free(&outputs);
  arena_free(&arena);
  free(leaked_file);
  free(generated_file);
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// A gallery that ships no `static` directory builds. `static_dir` is optional with a default, so an
// absent one is skipped rather than reported.
static void test_builds_with_no_static_dir(void) {
  char root_dir_template[] = "/tmp/fram-build-no-static.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(!fixture_path_exists(root_dir, "static"))) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  TEST_CHECK(fixture_path_exists(root_dir, "public/index.html"));
  TEST_CHECK(fixture_path_exists(root_dir, "public/album/index.html"));
  TEST_CHECK(fixture_path_exists(root_dir, "public/_fram/originals/album/one-jpg.jpg"));

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// A `static` directory is mirrored into `output_dir` as-is, so a top-level file lands at the output
// root and a nested one keeps its directories.
static void test_copies_static_files_when_present(void) {
  char root_dir_template[] = "/tmp/fram-build-static.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "static/robots.txt", "User-agent: *\n") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "static/assets/fram.css", "body{}\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  TEST_CHECK(fixture_path_exists(root_dir, "public/robots.txt"));
  TEST_CHECK(fixture_path_exists(root_dir, "public/assets/fram.css"));

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// Same-named sources in different directories publish to distinct, non-colliding outputs. One file
// name repeated in three nested albums is not a collision, because the album slug is part of every
// output path. Each directory level still gets its own album page.
static void test_distinguishes_same_name_in_different_dirs(void) {
  char root_dir_template[] = "/tmp/fram-build-nested.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/one.jpg") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/trip/one.jpg") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/trip/deep/one.jpg") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  TEST_CHECK(fixture_path_exists(root_dir, "public/trip/index.html"));
  TEST_CHECK(fixture_path_exists(root_dir, "public/trip/deep/index.html"));
  TEST_CHECK(fixture_path_exists(root_dir, "public/_fram/originals/one-jpg.jpg"));
  TEST_CHECK(fixture_path_exists(root_dir, "public/_fram/originals/trip/one-jpg.jpg"));
  TEST_CHECK(fixture_path_exists(root_dir, "public/_fram/originals/trip/deep/one-jpg.jpg"));

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// An album holding only sub-directories has no media of its own. Its own page is still written, and
// the zero-length media and sub-album arrays it allocates are valid rather than a failure.
static void test_builds_an_album_with_no_media_of_its_own(void) {
  char root_dir_template[] = "/tmp/fram-build-container.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/container/inner/one.jpg") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  TEST_CHECK(fixture_path_exists(root_dir, "public/index.html"));
  TEST_CHECK(fixture_path_exists(root_dir, "public/container/index.html"));
  TEST_CHECK(fixture_path_exists(root_dir, "public/container/inner/index.html"));

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// A second build over a finished output tree succeeds and produces the same set of outputs.
static void test_rebuild_succeeds_and_repeats_its_outputs(void) {
  char root_dir_template[] = "/tmp/fram-build-rebuild.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  static const char* const all_suffixes[] = {""};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  struct Arena arena;
  arena_init(&arena);
  struct PathList outputs;
  path_list_init(&outputs);
  char* output_dir = NULL;
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");

  output_dir = path_join(root_dir, "public", &arena);
  if (!TEST_CHECK(output_dir != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(fs_list_files_with_suffixes(&outputs, output_dir, NULL, all_suffixes, 1, false, NULL,
                                         0) == 0);
  // The album page, the root page, both derivatives and the copied original: the second build adds
  // nothing and removes nothing.
  TEST_CHECK(outputs.count == 5);

cleanup:
  path_list_free(&outputs);
  arena_free(&arena);
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// An `input_dir` inside `output_dir` builds, because no output lands inside it: `output_dir = "."`
// with `input_dir = "photos"` puts the gallery beside its sources. A rebuild succeeds and repeats
// the same outputs, so neither walk reads the first build's derivatives back as media.
static void test_builds_and_rebuilds_with_output_dir_holding_input_dir(void) {
  char root_dir_template[] = "/tmp/fram-build-dot-output.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  static const char* const all_suffixes[] = {""};
  size_t output_counts[2] = {0, 0};
  if (!TEST_CHECK(write_gallery_fixture(root_dir, "output_dir = \".\"\n") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }

  for (size_t i = 0; i < 2; i++) {
    struct StringBuffer error_buffer;
    string_buffer_init(&error_buffer);
    TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
    TEST_CHECK(error_buffer.len == 0);
    TEST_MSG("build %zu: '%s'", i + 1, error_buffer.data != NULL ? error_buffer.data : "");
    string_buffer_free(&error_buffer);

    struct PathList outputs;
    path_list_init(&outputs);
    TEST_CHECK(fs_list_files_with_suffixes(&outputs, root_dir, NULL, all_suffixes, 1, false, NULL,
                                           0) == 0);
    output_counts[i] = outputs.count;
    path_list_free(&outputs);
  }
  // The config, the template, and the source, then the album page, the root page, both derivatives
  // and the copied original.
  TEST_CHECK(output_counts[0] == 8);
  TEST_CHECK(output_counts[1] == output_counts[0]);
  TEST_CHECK(fixture_path_exists(root_dir, "index.html"));
  TEST_CHECK(fixture_path_exists(root_dir, "album/index.html"));
  TEST_CHECK(fixture_path_exists(root_dir, "_fram/originals/album/one-jpg.jpg"));

cleanup:
  remove_fixture_tree(root_dir);
}

// An `output_dir` that a symlink inside `input_dir` reaches is left out of the media walk, so a
// rebuild does not read the first build's outputs back as sources. The derivatives here are JPEG
// files, which the walk would otherwise discover as media.
static void test_rebuild_skips_output_dir_linked_from_input_dir(void) {
  char root_dir_template[] = "/tmp/fram-build-linked-output.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  static const char* const all_suffixes[] = {""};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  struct Arena arena;
  arena_init(&arena);
  struct PathList outputs;
  path_list_init(&outputs);
  char* link_path = NULL;
  char* output_dir = NULL;
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }
  link_path = path_join(root_dir, "photos/published", &arena);
  if (!TEST_CHECK(link_path != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(symlink("../public", link_path) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");

  output_dir = path_join(root_dir, "public", &arena);
  if (!TEST_CHECK(output_dir != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(fs_list_files_with_suffixes(&outputs, output_dir, NULL, all_suffixes, 1, false, NULL,
                                         0) == 0);
  // The album page, the root page, both derivatives and the copied original, and no album for the
  // linked output tree.
  TEST_CHECK(outputs.count == 5);
  TEST_CHECK(!fixture_path_exists(root_dir, "public/published"));

cleanup:
  path_list_free(&outputs);
  arena_free(&arena);
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// A caller-supplied `worker_count` reaches every pool instead of being replaced by the detected
// default. A build with it produces the same bytes as the default build. The resolved count is not
// observable from the return value, so it is read off the verbose phase lines, which are the only
// place the module reports it. Each parallel phase closes with its one-job progress line.
static void test_honors_requested_worker_count(void) {
  char root_dir_template[] = "/tmp/fram-build-workers.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  const struct BuildOptions options = {.worker_count = 3, .is_verbose = true};
  const struct BuildOptions single_worker = {.worker_count = 1, .is_verbose = true};
  char stdout_out[ERROR_MESSAGE_SIZE];
  char stderr_out[ERROR_MESSAGE_SIZE * 8];
  char expected[ERROR_MESSAGE_SIZE * 2];
  int expected_len = 0;
  unsigned char* generated = NULL;
  size_t generated_len = 0;
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/one.jpg") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(run_build_capturing(root_dir, &options, stdout_out, sizeof(stdout_out), stderr_out,
                                 sizeof(stderr_out)) == EXIT_CODE_OK);
  TEST_CHECK(stdout_out[0] == '\0');
  expected_len = snprintf(expected, sizeof(expected),
                          "loading config\n"
                          "discovering media\n"
                          "discovering static files\n"
                          "planning albums\n"
                          "probing media, workers: %d\n"
                          "\rprobing media 1/1\n"
                          "building output manifest\n"
                          "generating derivatives, workers: %d\n"
                          "\rgenerating derivatives 1/1\n"
                          "rendering albums, workers: %d\n"
                          "\rrendering albums 1/1\n"
                          "writing album pages\n"
                          "rendering aggregate templates\n"
                          "copying static files\n"
                          "build complete\n",
                          3, 3, 3);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  // One worker, requested explicitly, must be honored rather than read as unset. A guard spelled
  // `worker_count != 1` instead of `!= 0` swallows this request and falls back to the detected core
  // count. Asking for `3` above cannot see that. It is also the only way to ask for a serial build.
  // Like the `3` case, this assumes the host reports more than one core.
  TEST_CHECK(run_build_capturing(root_dir, &single_worker, stdout_out, sizeof(stdout_out),
                                 stderr_out, sizeof(stderr_out)) == EXIT_CODE_OK);
  TEST_CHECK(stdout_out[0] == '\0');
  expected_len = snprintf(expected, sizeof(expected),
                          "loading config\n"
                          "discovering media\n"
                          "discovering static files\n"
                          "planning albums\n"
                          "probing media, workers: %d\n"
                          "\rprobing media 1/1\n"
                          "building output manifest\n"
                          "generating derivatives, workers: %d\n"
                          "\rgenerating derivatives 1/1\n"
                          "rendering albums, workers: %d\n"
                          "\rrendering albums 1/1\n"
                          "writing album pages\n"
                          "rendering aggregate templates\n"
                          "copying static files\n"
                          "build complete\n",
                          1, 1, 1);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  // The requested count changes only how the work is scheduled, never the output.
  TEST_CHECK(read_fixture_file(root_dir, "public/index.html", &generated, &generated_len) == 0);
  TEST_CHECK(
      generated != NULL &&
      strcmp(
          (const char*)generated,
          "<!doctype html><title>Test Gallery - Test Gallery</title><ul><li>one.jpg</li></ul>") ==
          0);

cleanup:
  free(generated);
  remove_fixture_tree(root_dir);
}

// A gallery with no media skips the probe and derivative phases, so the verbose log carries no
// status or progress line for either. A phase with no jobs returns before it prints, so an empty
// gallery does not report a worker count for work it never starts. The album page phase still runs,
// for the root album alone.
static void test_skips_empty_phases_in_verbose_output(void) {
  char root_dir_template[] = "/tmp/fram-build-empty.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  const struct BuildOptions options = {.worker_count = 3, .is_verbose = true};
  char stdout_out[ERROR_MESSAGE_SIZE];
  char stderr_out[ERROR_MESSAGE_SIZE * 4];
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  // A file that is not media keeps `input_dir` present while the gallery holds no media.
  if (!TEST_CHECK(write_fixture_file(root_dir, "photos/notes.txt", "not media\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(run_build_capturing(root_dir, &options, stdout_out, sizeof(stdout_out), stderr_out,
                                 sizeof(stderr_out)) == EXIT_CODE_OK);
  TEST_CHECK(stdout_out[0] == '\0');
  TEST_CHECK(strcmp(stderr_out,
                    "loading config\n"
                    "discovering media\n"
                    "discovering static files\n"
                    "planning albums\n"
                    "building output manifest\n"
                    "rendering albums, workers: 3\n"
                    "\rrendering albums 1/1\n"
                    "writing album pages\n"
                    "rendering aggregate templates\n"
                    "copying static files\n"
                    "build complete\n") == 0);
  TEST_MSG("actual: '%s'", stderr_out);

cleanup:
  remove_fixture_tree(root_dir);
}

// A build that succeeds leaves the collected diagnostic buffer empty, which is the other half of
// `cmd_build_execute`'s `error_out` contract. Without this a phase that appended to `error_out` on
// the success path would go unnoticed, since every other test that inspects the buffer is a failure
// test.
static void test_leaves_error_buffer_empty_on_success(void) {
  char root_dir_template[] = "/tmp/fram-build-clean.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// `cmd_build_run` reports failure as `EXIT_CODE_FAILURE` and prints the collected diagnostic to
// `stderr` exactly once, which is the whole of what it adds over `cmd_build_execute`. Every other
// test routes failures through `cmd_build_execute` to inspect the message, so without this the
// boundary itself is unasserted: the exit code and the single print.
static void test_run_prints_diagnostic_once_and_fails(void) {
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('fram.toml')\n",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));

  char root_dir_template[] = "/tmp/fram-build-exit.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  // No `fram.toml`, the earliest failure, so the diagnostic is a single known line.
  const struct BuildOptions options = {0};
  char stdout_out[ERROR_MESSAGE_SIZE];
  char stderr_out[ERROR_MESSAGE_SIZE * 4];

  TEST_CHECK(run_build_capturing(root_dir, &options, stdout_out, sizeof(stdout_out), stderr_out,
                                 sizeof(stderr_out)) == EXIT_CODE_FAILURE);
  TEST_CHECK(stdout_out[0] == '\0');
  // Compared whole rather than by substring: "exactly once" is the claim, and only a whole-buffer
  // comparison can tell one print from two.
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  remove_fixture_tree(root_dir);
}

// A missing `fram.toml` is reported at the command boundary rather than printed by a helper.
static void test_reports_missing_config(void) {
  // The cause is part of the claim: a missing config must not read like an unreadable one.
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('fram.toml')",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));

  char root_dir_template[] = "/tmp/fram-build-no-config.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);

  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// `input_dir` is required, so an absent one is a failure, reported as a resolution failure carrying
// the system cause. `ENOENT` and `EACCES` need different fixes and nothing later would report
// either, so the cause has to appear here. Only the optional read root is skipped when missing.
// Compared exactly, so a caller that appended the path a second time would fail.
static void test_reports_absent_input_dir(void) {
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected),
                                    "failed to resolve config directory 'input_dir': %s ('photos')",
                                    error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));

  char root_dir_template[] = "/tmp/fram-build-no-input.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// `templates_dir` is required the same way, and reports the key the user has to fix rather than the
// one that happened to be checked first.
static void test_reports_absent_templates_dir(void) {
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to resolve config directory 'templates_dir': %s ('templates')",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));

  char root_dir_template[] = "/tmp/fram-build-no-templates.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_fixture_file(root_dir, "fram.toml", GALLERY_CONFIG) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// A `templates_dir` that exists but is a regular file is reported as the wrong type, which is its
// own cause rather than an `errno`, so the message carries no system text.
static void test_reports_templates_dir_that_is_a_file(void) {
  char root_dir_template[] = "/tmp/fram-build-templates-file.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_fixture_file(root_dir, "fram.toml", GALLERY_CONFIG) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates", "not a directory\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  TEST_CHECK(error_buffer.data != NULL &&
             strcmp(error_buffer.data,
                    "failed to resolve config directory 'templates_dir': not a directory "
                    "('templates')") == 0);

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// A configured `output_dir` that already exists as a regular file is reported once the manifest has
// passed, where the build creates `output_dir`, before any derivative is written. The reason names
// the component that failed and the caller does not repeat the configured root, so the message is
// compared whole.
static void test_reports_unusable_output_dir(void) {
  char root_dir_template[] = "/tmp/fram-build-output-file.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }
  // `public` is the default `output_dir`, so a file there is what makes `fs_mkdir_p` fail.
  if (!TEST_CHECK(write_fixture_file(root_dir, "public", "not a directory") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  TEST_CHECK(
      error_buffer.data != NULL &&
      strcmp(error_buffer.data,
             "failed to prepare output directory: exists and is not a directory ('public')") == 0);

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// An `output_dir` that is a dangling symlink is reported once the manifest has passed, where the
// build creates `output_dir`, before any derivative is written. A dangling symlink has no identity,
// so the manifest sees nothing there, and `fs_mkdir_p` then fails the `stat` that follows `mkdir`'s
// `EEXIST`. The message is compared whole.
static void test_reports_dangling_output_dir_symlink(void) {
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to prepare output directory: cannot inspect directory: %s ('public')",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));

  char root_dir_template[] = "/tmp/fram-build-output-symlink.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  struct Arena arena;
  arena_init(&arena);
  char* output_dir = NULL;
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }
  // `public` is the default `output_dir`, so a dangling symlink there is what makes `fs_mkdir_p`
  // fail.
  output_dir = path_join(root_dir, "public", &arena);
  if (!TEST_CHECK(output_dir != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(symlink("missing", output_dir) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");

cleanup:
  arena_free(&arena);
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// An `output_dir` below `input_dir` is refused before media is discovered or probed, so no earlier
// build's output can be read back as a source, and it is refused although it does not exist yet.
// The verbose phase lines show that the build stopped before the media walk, and the refused build
// creates none of the nested directories.
static void test_rejects_output_dir_inside_input_dir(void) {
  char root_dir_template[] = "/tmp/fram-build-nested-output.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  const struct BuildOptions options = {.is_verbose = true};
  char stdout_out[ERROR_MESSAGE_SIZE];
  char stderr_out[ERROR_MESSAGE_SIZE * 4];
  if (!TEST_CHECK(write_gallery_fixture(root_dir, "output_dir = \"photos/generated/site\"\n") ==
                  0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(run_build_capturing(root_dir, &options, stdout_out, sizeof(stdout_out), stderr_out,
                                 sizeof(stderr_out)) == EXIT_CODE_FAILURE);
  TEST_CHECK(stdout_out[0] == '\0');
  TEST_CHECK(strcmp(stderr_out,
                    "loading config\n"
                    "output directory would write inside 'input_dir': "
                    "'photos/generated/site'\n") == 0);
  TEST_MSG("actual: '%s'", stderr_out);
  TEST_CHECK(!fixture_path_exists(root_dir, "photos/generated"));

cleanup:
  remove_fixture_tree(root_dir);
}

// Slugging is lossy, so two different source names can claim one output path. The manifest reports
// the collision and names both sources instead of letting the second write win. A non-ASCII byte
// folds to hex, so `caf\xC3\xA9` produces the slug a literal `cafc3a9` also produces. The refused
// build creates no `output_dir`, because nothing is written before the manifest passes.
static void test_rejects_colliding_source_names(void) {
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               "photos/album/cafc3a9.jpg", "photos/album/caf\xC3\xA9.jpg",
               "public/_fram/v1-s-8x8c-q70-m-16x16-q75-f1000/album/cafc3a9-jpg-s.jpg");
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));

  char root_dir_template[] = "/tmp/fram-build-source-collision.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/caf\xC3\xA9.jpg") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/cafc3a9.jpg") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_CHECK(!fixture_path_exists(root_dir, "public"));

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// A static file copied to the output root shares its paths with the generated pages, so one named
// after the root album page is rejected as a duplicate of it rather than overwriting it or being
// overwritten. The refused build creates no `output_dir`.
static void test_rejects_static_file_colliding_with_album_page(void) {
  char root_dir_template[] = "/tmp/fram-build-static-collision.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "static/index.html", "static\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data,
                                                 "duplicate output path for 'photos' and "
                                                 "'static/index.html': 'public/index.html'") == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  TEST_CHECK(!fixture_path_exists(root_dir, "public"));

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// An album template that fails to parse fails the build at the page phase. The diagnostic carries
// the parser's cause, its position and the template name, followed by the album that was being
// rendered, so a template error reads apart from a template that cannot be read. No other test here
// fails at the page phase, so without this `cmd_build_execute`'s page-render arm never runs.
static void test_reports_bad_template(void) {
  char root_dir_template[] = "/tmp/fram-build-bad-template.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, "album_template = \"broken.html\"\n") == 0)) {
    goto cleanup;
  }
  // The section never closes, so the template is read but fails to compile.
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/broken.html", "{{#media}}unclosed") ==
                  0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/one.jpg") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(error_buffer.data != NULL &&
             strcmp(error_buffer.data,
                    "section-opening tag has no closer at line 1, column 1 (in 'broken.html') "
                    "(while rendering the root album)") == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// Two media items that both fail to probe append one diagnostic line each, so neither is lost to
// the other. This is the postcondition `cmd_build_execute` states and the reason `error_out` is
// growable rather than a fixed buffer: an `append_error` that overwrote, or that dropped the
// separator and ran two messages together, would still satisfy every single-item test.
static void test_reports_one_line_per_failing_media_item(void) {
  char root_dir_template[] = "/tmp/fram-build-probe-failures.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  // Not JPEG data, so the probe fails for both sources.
  if (!TEST_CHECK(write_fixture_file(root_dir, "photos/album/a.jpg", "not a JPEG\n") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "photos/album/b.jpg", "not a JPEG\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  // Sources are walked in sorted order, so `a.jpg` precedes `b.jpg`. Compared whole, with the
  // separator in the middle, so a dropped newline or a lost line both fail here.
  TEST_CHECK(error_buffer.data != NULL &&
             strcmp(error_buffer.data,
                    "failed to probe media: failed to probe JPEG: unknown image type "
                    "('photos/album/a.jpg')\n"
                    "failed to probe media: failed to probe JPEG: unknown image type "
                    "('photos/album/b.jpg')") == 0);

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// An output path that cannot be opened as a file fails the build at the write phase, after every
// album has rendered successfully. That is the last of `cmd_build_execute`'s phase arms and the
// only one reached with a complete set of rendered pages in hand, so a write failure reported as
// success would otherwise pass the suite with the pages silently missing.
static void test_reports_unwritable_output(void) {
  char reason[FS_REASON_SIZE];
  error_system_message(reason, sizeof(reason), EISDIR);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to write output: %s (for 'album', to 'public/album/index.html')", reason);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));

  char root_dir_template[] = "/tmp/fram-build-unwritable.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  struct Arena arena;
  arena_init(&arena);
  char* output_path = NULL;
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }
  output_path = path_join(root_dir, "public/album/index.html", &arena);
  if (!TEST_CHECK(output_path != NULL)) {
    goto cleanup;
  }
  // A directory at the page path cannot be opened as a file even by a privileged process.
  TEST_CHECK(fs_mkdir_p(output_path, NULL, 0) == 0);

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);

cleanup:
  arena_free(&arena);
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

// A `static_dir` that exists but is a regular file is reported. Skipping an absent optional root
// must not also skip a misconfigured one, which is why the existence predicate asks whether
// anything is there rather than whether a directory is there: the wrong type survives the skip and
// reaches `fs_require_dir`, which names it.
static void test_reports_static_dir_that_is_a_file(void) {
  char root_dir_template[] = "/tmp/fram-build-static-file.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_gallery_fixture(root_dir, NULL) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_jpeg(root_dir, "photos/album/one.jpg") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "static", "not a directory\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  TEST_CHECK(error_buffer.data != NULL &&
             strcmp(error_buffer.data,
                    "failed to resolve config directory 'static_dir': not a directory "
                    "('static')") == 0);

cleanup:
  string_buffer_free(&error_buffer);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"honors configured album template", test_honors_configured_album_template},
    {"writes exactly manifest outputs", test_writes_exactly_manifest_outputs},
    {"tolerates trailing slash on input_dir", test_tolerates_trailing_slash_on_input_dir},
    {"builds with no static dir", test_builds_with_no_static_dir},
    {"copies static files when present", test_copies_static_files_when_present},
    {"distinguishes same name in different dirs", test_distinguishes_same_name_in_different_dirs},
    {"builds an album with no media of its own", test_builds_an_album_with_no_media_of_its_own},
    {"rebuild succeeds and repeats its outputs", test_rebuild_succeeds_and_repeats_its_outputs},
    {"builds and rebuilds with output dir holding input dir",
     test_builds_and_rebuilds_with_output_dir_holding_input_dir},
    {"rebuild skips output dir linked from input dir",
     test_rebuild_skips_output_dir_linked_from_input_dir},
    {"honors requested worker count", test_honors_requested_worker_count},
    {"skips empty phases in verbose output", test_skips_empty_phases_in_verbose_output},
    {"leaves error buffer empty on success", test_leaves_error_buffer_empty_on_success},
    {"run prints diagnostic once and fails", test_run_prints_diagnostic_once_and_fails},
    {"reports missing config", test_reports_missing_config},
    {"reports absent input dir", test_reports_absent_input_dir},
    {"reports absent templates dir", test_reports_absent_templates_dir},
    {"reports templates dir that is a file", test_reports_templates_dir_that_is_a_file},
    {"reports unusable output dir", test_reports_unusable_output_dir},
    {"reports dangling output dir symlink", test_reports_dangling_output_dir_symlink},
    {"rejects output dir inside input dir", test_rejects_output_dir_inside_input_dir},
    {"rejects colliding source names", test_rejects_colliding_source_names},
    {"rejects static file colliding with album page",
     test_rejects_static_file_colliding_with_album_page},
    {"reports bad template", test_reports_bad_template},
    {"reports one line per failing media item", test_reports_one_line_per_failing_media_item},
    {"reports unwritable output", test_reports_unwritable_output},
    {"reports static dir that is a file", test_reports_static_dir_that_is_a_file},
    {NULL, NULL},
};
