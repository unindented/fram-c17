#include <acutest.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build/page_renderer.h"
#include "build/site_writer.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few hundred bytes. */
enum { TEST_FILE_LEN_MAX = 1024 * 1024 };

/**
 * @brief Checks that one fixture file holds exactly the expected text.
 *
 * @param path     File to read. Must not be `NULL`.
 * @param expected Terminated text the file must hold byte for byte. Must not be `NULL`.
 */
static void check_file_text(const char* path, const char* expected) {
  unsigned char* bytes = NULL;
  size_t bytes_len = 0;
  char reason[FS_REASON_SIZE];
  if (!TEST_CHECK(
          fs_read_file(path, TEST_FILE_LEN_MAX, &bytes, &bytes_len, reason, sizeof(reason)) == 0)) {
    return;
  }
  TEST_CHECK(bytes_len == strlen(expected));
  TEST_CHECK(strcmp((const char*)bytes, expected) == 0);
  free(bytes);
}

// Album pages, aggregate templates, and nested assets land at their planned destinations.
static void test_writes_complete_output_layer(void) {
  char root_dir_template[] = "/tmp/fram-writer-output-layer.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena paths;
  arena_init(&paths);
  char* output_dir = path_join(root_dir, "public", &paths);
  char* templates_dir = path_join(root_dir, "templates", &paths);
  char* assets_dir = path_join(root_dir, "assets", &paths);
  char* root_output = path_join(output_dir, "index.html", &paths);
  char* asset_source = path_join(assets_dir, "icons/a.txt", &paths);
  char* aggregate_output = path_join(output_dir, "map.xml", &paths);
  char* nested_output = path_join(output_dir, "nested/links.html", &paths);
  char* asset_output = path_join(output_dir, "assets/icons/a.txt", &paths);
  static const char* const aggregates[] = {"map.xml", "nested/links.html"};
  struct GalleryConfig config;
  gallery_config_init(&config);
  config.title = "Gallery & Co";
  config.author = "Ada";
  config.output_dir = output_dir;
  config.templates_dir = templates_dir;
  config.assets_dir = assets_dir;
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 2;
  struct Album root = {.source_dir = "",
                       .title = "Root",
                       .url_path = "index.html",
                       .output_path = root_output,
                       .path_to_root = ""};
  const struct Album* albums[] = {&root};
  char html[] = "<h1>Root</h1>";
  const struct RenderedPage rendered[] = {{.html = html, .html_len = sizeof(html) - 1}};
  struct PathList assets;
  path_list_init(&assets);
  char err[ERROR_MESSAGE_SIZE];
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/map.xml", "<g>{{gallery.title}}</g>") ==
                  0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/nested/links.html", "{{album.url}}") ==
                  0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "assets/icons/a.txt", "asset") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(output_dir != NULL && templates_dir != NULL && assets_dir != NULL &&
                  root_output != NULL && asset_source != NULL && aggregate_output != NULL &&
                  nested_output != NULL && asset_output != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(path_list_push(&assets, asset_source) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(site_writer_write_album_pages(albums, 1, rendered, err, sizeof(err)) == 0);
  TEST_CHECK(site_writer_write_aggregates(&config, albums[0], err, sizeof(err)) == 0);
  TEST_CHECK(site_writer_copy_assets(&config, &assets, err, sizeof(err)) == 0);
  check_file_text(root_output, "<h1>Root</h1>");
  check_file_text(aggregate_output, "<g>Gallery &amp; Co</g>");
  check_file_text(nested_output, "../index.html");
  check_file_text(asset_output, "asset");

cleanup:
  path_list_free(&assets);
  gallery_config_free(&config);
  arena_free(&paths);
  remove_fixture_tree(root_dir);
}

// A failed page write names the root album in words rather than quoting its empty source directory,
// then the destination. The destination is an existing directory, so opening it for writing fails.
static void test_write_album_pages_names_root_album_in_words(void) {
  char root_dir_template[] = "/tmp/fram-writer-root-page.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Album root = {.source_dir = "", .title = "Root", .output_path = root_dir};
  const struct Album* albums[] = {&root};
  char html[] = "<h1>Root</h1>";
  const struct RenderedPage rendered[] = {{.html = html, .html_len = sizeof(html) - 1}};
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(site_writer_write_album_pages(albums, 1, rendered, err, sizeof(err)) == -1);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected),
                                    "failed to write output: %s (for the root album, to '%s')",
                                    error_system_message(reason, sizeof(reason), EISDIR), root_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("err: %s", err);

cleanup:
  remove_fixture_tree(root_dir);
}

// A failed sub-album page write quotes the album's source directory, even one literally named
// `root album`, so it never reads the same as the root's diagnostic.
static void test_write_album_pages_quotes_sub_album_source_dir(void) {
  char root_dir_template[] = "/tmp/fram-writer-sub-album-page.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Album album = {.source_dir = "root album", .title = "root album", .output_path = root_dir};
  const struct Album* albums[] = {&album};
  char html[] = "<h1>root album</h1>";
  const struct RenderedPage rendered[] = {{.html = html, .html_len = sizeof(html) - 1}};
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(site_writer_write_album_pages(albums, 1, rendered, err, sizeof(err)) == -1);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to write output: %s (for 'root album', to '%s')",
               error_system_message(reason, sizeof(reason), EISDIR), root_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("err: %s", err);

cleanup:
  remove_fixture_tree(root_dir);
}

// A failed aggregate write names the operation and the reason, then the destination. The
// destination is an existing directory, so opening it for writing fails.
static void test_write_aggregates_reports_write_failure(void) {
  char root_dir_template[] = "/tmp/fram-writer-aggregate.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena paths;
  arena_init(&paths);
  char* output_dir = path_join(root_dir, "public", &paths);
  char* templates_dir = path_join(root_dir, "templates", &paths);
  char* aggregate_output = path_join(output_dir, "map.xml", &paths);
  static const char* const aggregates[] = {"map.xml"};
  struct GalleryConfig config;
  gallery_config_init(&config);
  config.output_dir = output_dir;
  config.templates_dir = templates_dir;
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  struct Album root = {.source_dir = "", .title = "Root", .path_to_root = ""};
  char err[ERROR_MESSAGE_SIZE] = "";
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = -1;
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/map.xml", "<g></g>") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "public/map.xml/keep", "") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(output_dir != NULL && templates_dir != NULL && aggregate_output != NULL)) {
    goto cleanup;
  }

  TEST_CHECK(site_writer_write_aggregates(&config, &root, err, sizeof(err)) == -1);
  expected_len = snprintf(expected, sizeof(expected), "failed to write template output: %s ('%s')",
                          error_system_message(reason, sizeof(reason), EISDIR), aggregate_output);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("err: %s", err);

cleanup:
  gallery_config_free(&config);
  arena_free(&paths);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"writes complete output layer", test_writes_complete_output_layer},
    {"write album pages names root album in words",
     test_write_album_pages_names_root_album_in_words},
    {"write album pages quotes sub album source dir",
     test_write_album_pages_quotes_sub_album_source_dir},
    {"write aggregates reports write failure", test_write_aggregates_reports_write_failure},
    {NULL, NULL},
};
