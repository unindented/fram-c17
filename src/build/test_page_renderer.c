#include <acutest.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build/page_renderer.h"
#include "core/error.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "runtime/fs.h"
#include "shared/string_buffer.h"
#include "test_support.h"

// Parallel jobs render each album into its corresponding output slot with independent context.
static void test_renders_pages_by_index(void) {
  char root_dir_template[] = "/tmp/fram-page-indexes.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct GalleryConfig config;
  gallery_config_init(&config);
  config.title = "Gallery";
  config.author = "Ada";
  config.templates_dir = root_dir;
  struct Album root = {.source_dir = "", .title = "Root", .path_to_root = ""};
  struct Album child = {.source_dir = "trip", .title = "Trip", .path_to_root = "../"};
  const struct Album* albums[] = {&root, &child};
  struct RenderedPage rendered[2] = {0};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (!TEST_CHECK(write_fixture_file(root_dir, "album.html", "{{album.title}}:{{path_to_root}}") ==
                  0)) {
    goto cleanup;
  }

  TEST_CHECK(page_renderer_render_pages(&config, albums, 2, 2, false, rendered, &error_buffer) ==
             0);
  if (!TEST_CHECK(rendered[0].html != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(rendered[1].html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered[0].html, "Root:") == 0);
  TEST_CHECK(rendered[0].html_len == strlen("Root:"));
  TEST_CHECK(strcmp(rendered[1].html, "Trip:../") == 0);
  TEST_CHECK(rendered[1].html_len == strlen("Trip:../"));
  TEST_CHECK(error_buffer.len == 0);

cleanup:
  free(rendered[0].html);
  free(rendered[1].html);
  string_buffer_free(&error_buffer);
  gallery_config_free(&config);
  remove_fixture_tree(root_dir);
}

// A missing album template surfaces a per-album diagnostic in the collected error buffer. The root
// album is named in words rather than by its empty source directory.
static void test_reports_missing_template(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  config.title = "Gallery";
  config.author = "Ada";
  config.templates_dir = "/does/not/exist";
  struct Album root = {.source_dir = "", .title = "Root", .path_to_root = ""};
  const struct Album* albums[] = {&root};
  struct RenderedPage rendered[1] = {0};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(page_renderer_render_pages(&config, albums, 1, 1, false, rendered, &error_buffer) ==
             -1);
  TEST_CHECK(rendered[0].html == NULL);
  // The diagnostic names the failing album, the template file that could not be read, and why.
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read template: %s ('/does/not/exist/%s') (while rendering the root "
               "album)",
               error_system_message(reason, sizeof(reason), ENOENT), config.album_template);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");

  string_buffer_free(&error_buffer);
  gallery_config_free(&config);
}

// A missing album template reported for a sub-album quotes its source directory, even one literally
// named `root album`, so it never reads the same as the root's attribution.
static void test_reports_missing_template_for_sub_album(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  config.title = "Gallery";
  config.author = "Ada";
  config.templates_dir = "/does/not/exist";
  struct Album album = {.source_dir = "root album", .title = "root album", .path_to_root = "../"};
  const struct Album* albums[] = {&album};
  struct RenderedPage rendered[1] = {0};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(page_renderer_render_pages(&config, albums, 1, 1, false, rendered, &error_buffer) ==
             -1);
  TEST_CHECK(rendered[0].html == NULL);
  // The diagnostic names the failing album, the template file that could not be read, and why.
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read template: %s ('/does/not/exist/%s') (while rendering album "
               "'root album')",
               error_system_message(reason, sizeof(reason), ENOENT), config.album_template);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");

  string_buffer_free(&error_buffer);
  gallery_config_free(&config);
}

TEST_LIST = {
    {"renders pages by index", test_renders_pages_by_index},
    {"reports missing template", test_reports_missing_template},
    {"reports missing template for sub album", test_reports_missing_template_for_sub_album},
    {NULL, NULL},
};
