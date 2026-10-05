#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "build/template.h"
#include "core/error.h"
#include "core/fram_version.h"
#include "core/path.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/media_item.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "test_support.h"

/**
 * @brief Initializes shared gallery metadata for template tests.
 *
 * The assigned fields are string literals, and `gallery_config_free` releases only arena storage.
 *
 * @param gallery_config Configuration to initialize. Must not be `NULL`.
 */
static void init_test_gallery_config(struct GalleryConfig* gallery_config) {
  gallery_config_init(gallery_config);
  gallery_config->title = "Gallery";
  gallery_config->author = "Ada";
}

/**
 * @brief Creates a fixture root holding one `album.html` template.
 *
 * The returned pointer aliases the caller's writable template and must not be freed.
 *
 * @param root_dir Writable `mkdtemp` template. Receives the created directory path. Must not be
 *                 `NULL`.
 * @param text     Terminated template text to write.
 * @return `root_dir` on success, or `NULL` after recording a test-plumbing failure, with no fixture
 *         left behind.
 */
static const char* write_template_fixture(char root_dir[static 1], const char* text) {
  const char* created_root_dir = init_fixture_dir(root_dir);
  if (created_root_dir == NULL) {
    return NULL;
  }
  if (!TEST_CHECK(write_fixture_file(created_root_dir, "album.html", text) == 0)) {
    remove_fixture_tree(created_root_dir);
    return NULL;
  }
  return created_root_dir;
}

/**
 * @brief Writes one fixture file whose bytes may include a `NUL`.
 *
 * @param root_dir      Fixture root directory.
 * @param relative_path Relative fixture path below `root_dir`.
 * @param data          Bytes to write.
 * @param data_len      Number of bytes in `data`.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
static int write_fixture_bytes(const char* root_dir,
                               const char* relative_path,
                               const char* data,
                               size_t data_len) {
  char fixture_path[256];
  const int n = snprintf(fixture_path, sizeof(fixture_path), "%s/%s", root_dir, relative_path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(fixture_path));
  if (n <= 0 || (size_t)n >= sizeof(fixture_path)) {
    return -1;
  }
  return fs_write_file(fixture_path, data, data_len, NULL, 0);
}

// Album, breadcrumb, repeated-media, boolean, escaping, and contextual URL nodes resolve together.
static void test_renders_gallery_tree_with_page_relative_urls(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* text =
      "{{gallery.title}}|{{album.title}}|{{#breadcrumbs}}[{{title}}={{url}}]{{/breadcrumbs}}|"
      "{{#media}}{{title}}={{s.url}}/{{s.width}}x{{s.height}},"
      "{{card.url}}/{{card.width}}x{{card.height}},{{original_url}}{{#is_image}}I{{/is_image}};"
      "{{/media}}";
  const char* root_dir = write_template_fixture(root_dir_template, text);
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  gallery_config_init(&gallery_config);
  gallery_config.title = "A & B";
  gallery_config.author = "Ada";
  gallery_config.templates_dir = root_dir;

  struct Album root = {
      .source_dir = "", .title = "Root", .url_path = "index.html", .path_to_root = "", .depth = 0};
  struct MediaDerivative derivatives[] = {
      {.name = "s", .url = "_fram/r/s/x.jpg", .width_px = 120, .height_px = 120},
      {.name = "card", .url = "_fram/r/card/x.jpg", .width_px = 480, .height_px = 320},
      // A configured name cannot shadow a built-in media field.
      {.name = "title", .url = "_fram/r/title/x.jpg", .width_px = 1, .height_px = 1},
  };
  struct MediaItem media = {.title = "x<y.jpg",
                            .kind = MEDIA_KIND_IMAGE,
                            .derivatives = derivatives,
                            .derivative_count = 3,
                            .original_url = "_fram/originals/x.jpg"};
  struct MediaItem* media_items[] = {&media};
  struct Album child = {.source_dir = "trip",
                        .title = "Trip",
                        .url_path = "trip/index.html",
                        .path_to_root = "../",
                        .media = media_items,
                        .media_count = 1,
                        .parent = &root,
                        .depth = 1};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &child};
  char* rendered_html = template_render_file(root_dir, "album.html", &context, NULL, NULL, 0);
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html,
                    "A &amp; B|Trip|[Root=../index.html][Trip=../trip/index.html]|"
                    "x&lt;y.jpg=../_fram/r/s/x.jpg/120x120,"
                    "../_fram/r/card/x.jpg/480x320,"
                    "../_fram/originals/x.jpgI;") == 0);

cleanup:
  free(rendered_html);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// Sub-album links and inherited covers are materialized relative to the page being rendered.
static void test_renders_sub_album_cover(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir =
      write_template_fixture(root_dir_template,
                             "{{#sub_albums}}{{title}}={{url}}={{cover.s.url}}={{cover.s.width}}x"
                             "{{cover.s.height}}={{item_count}};"
                             "{{/sub_albums}}");
  if (root_dir == NULL) {
    return;
  }
  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct MediaDerivative cover_derivatives[] = {
      {.name = "s", .url = "_fram/r/s/trip/x.jpg", .width_px = 120, .height_px = 120},
  };
  struct MediaItem cover = {.derivatives = cover_derivatives, .derivative_count = 1};
  struct Album child = {.title = "Trip",
                        .url_path = "trip/index.html",
                        .cover = &cover,
                        .item_count_total = 3,
                        .depth = 1};
  struct Album* children[] = {&child};
  struct Album root = {.title = "Root",
                       .url_path = "index.html",
                       .path_to_root = "",
                       .sub_albums = children,
                       .sub_album_count = 1};
  child.parent = &root;
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = template_render_file(root_dir, "album.html", &context, NULL, NULL, 0);
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "Trip=trip/index.html=_fram/r/s/trip/x.jpg=120x120=3;") == 0);

cleanup:
  free(rendered_html);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// `generator` names the tool and its version, at the root and inside a section.
static void test_renders_generator(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir =
      write_template_fixture(root_dir_template, "{{generator}}|{{#media}}{{generator}}{{/media}}");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct MediaItem item = {0};
  struct MediaItem* media[] = {&item};
  struct Album root = {.title = "Root",
                       .url_path = "index.html",
                       .path_to_root = "",
                       .media = media,
                       .media_count = 1};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = template_render_file(root_dir, "album.html", &context, NULL, NULL, 0);
  char expected[64];
  const int expected_len = snprintf(expected, sizeof(expected), "fram %s|fram %s",
                                    fram_version_string(), fram_version_string());
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, expected) == 0);
  TEST_MSG("rendered: '%s'", rendered_html);

cleanup:
  free(rendered_html);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// A safe partial loads and renders in the parent context, with its values still escaped.
static void test_renders_partial(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "before {{> card}} after");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root <One>", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = NULL;
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "{{album.title}}") == 0)) {
    goto cleanup;
  }
  rendered_html = template_render_file(root_dir, "album.html", &context, NULL, NULL, 0);
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "before Root &lt;One&gt; after") == 0);

cleanup:
  free(rendered_html);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// A partial referenced twice per media item across three items expands six times and renders each
// item's own content each time. The compile cache is not observable through the API, so this pins
// what a broken cache would break, stale or empty expansions, rather than the number of compiles.
static void test_repeated_partial_renders_same_content_each_time(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir =
      write_template_fixture(root_dir_template, "{{#media}}{{>card}}{{>card}}{{/media}}");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct MediaItem items[] = {{.title = "p1"}, {.title = "p2"}, {.title = "p3"}};
  struct MediaItem* media[] = {&items[0], &items[1], &items[2]};
  struct Album root = {.title = "Root",
                       .url_path = "index.html",
                       .path_to_root = "",
                       .media = media,
                       .media_count = 3};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  char* rendered_html = NULL;
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "[{{title}}]") == 0)) {
    goto cleanup;
  }
  rendered_html = template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err));
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(strcmp(rendered_html, "[p1][p1][p2][p2][p3][p3]") == 0);

cleanup:
  free(rendered_html);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// Referencing one partial more times than the distinct-partial limit allows still builds, because
// every reference after the first is a cache hit and only a miss consumes a slot. This is the
// assertion the sibling above cannot make: expansions are counted before the cache lookup, so a
// cache that never hit would still expand the right number of times and render the right bytes.
// Exhausting the distinct-partial budget is the only difference observable through the API, so the
// reference count sits above the limit rather than at two.
//
// `RENDER_PARTIAL_COUNT_MAX` is file-local to `template.c`, so it is spelled out here. Changing it
// there must update this. A cache that never hit would fail on reference 65.
static void test_repeated_partial_stays_under_distinct_limit(void) {
  enum { RENDER_PARTIAL_COUNT_MAX = 64 };
  enum { REFERENCE_COUNT = RENDER_PARTIAL_COUNT_MAX + 6 };
  enum { REFERENCE_LEN = sizeof("{{>card}}") - 1 };
  enum { EXPANSION_LEN = sizeof("[Root]") - 1 };
  char source[REFERENCE_COUNT * REFERENCE_LEN + 1];
  char expected[REFERENCE_COUNT * EXPANSION_LEN + 1];
  for (size_t i = 0; i < (size_t)REFERENCE_COUNT; i++) {
    memcpy(source + i * REFERENCE_LEN, "{{>card}}", REFERENCE_LEN);
    memcpy(expected + i * EXPANSION_LEN, "[Root]", EXPANSION_LEN);
  }
  source[REFERENCE_COUNT * REFERENCE_LEN] = '\0';
  expected[REFERENCE_COUNT * EXPANSION_LEN] = '\0';

  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, source);
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  char* rendered_html = NULL;
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "[{{album.title}}]") == 0)) {
    goto cleanup;
  }
  rendered_html = template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err));
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(strcmp(rendered_html, expected) == 0);

cleanup:
  free(rendered_html);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// An empty template returns an allocated empty string.
static void test_empty_template_yields_empty_string(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = template_render_file(root_dir, "album.html", &context, NULL, NULL, 0);
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "") == 0);

cleanup:
  free(rendered_html);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// An unknown variable renders as empty text per the Mustache spec, at the root and in a subtree.
static void test_unknown_variable_renders_empty(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "[{{typo}}|{{gallery.typo}}]\n");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = template_render_file(root_dir, "album.html", &context, NULL, NULL, 0);
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "[|]\n") == 0);

cleanup:
  free(rendered_html);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// An inverted section renders only when the list is empty.
static void test_inverted_section_renders_when_list_empty(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir =
      write_template_fixture(root_dir_template, "{{#media}}x{{/media}}{{^media}}none{{/media}}");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);

  struct Album empty = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context_empty = {.gallery_config = &gallery_config,
                                                .album_current = &empty};
  struct MediaItem item = {.title = "x.jpg"};
  struct MediaItem* media[] = {&item};
  struct Album full = {.title = "Root",
                       .url_path = "index.html",
                       .path_to_root = "",
                       .media = media,
                       .media_count = 1};
  const struct TemplateContext context_full = {.gallery_config = &gallery_config,
                                               .album_current = &full};
  char* rendered_html = template_render_file(root_dir, "album.html", &context_empty, NULL, NULL, 0);
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "none") == 0);
  free(rendered_html);

  rendered_html = template_render_file(root_dir, "album.html", &context_full, NULL, NULL, 0);
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "x") == 0);

cleanup:
  free(rendered_html);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// An empty field value is falsey as a section, so an unset field reads as absent. Were an empty
// scalar truthy, `{{#original_url}}` would render for every item and `{{^original_url}}` for none,
// so both branches are asserted here. The field is media-only, so an empty value cannot resolve
// through an outer context instead, as an empty media `title` would to the album's `title`.
// `gallery.base_url` is empty unless configured, so a template guarding it relies on this rule.
static void test_empty_field_is_falsey_as_section(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir =
      write_template_fixture(root_dir_template,
                             "{{#media}}[{{#original_url}}U:{{original_url}}{{/original_url}}"
                             "{{^original_url}}NONE{{/original_url}}]{{/media}}");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct MediaItem first = {.original_url = ""};
  struct MediaItem second = {.original_url = "_fram/originals/b.jpg"};
  struct MediaItem* media[] = {&first, &second};
  struct Album root = {.title = "Root",
                       .url_path = "index.html",
                       .path_to_root = "",
                       .media = media,
                       .media_count = 2};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = template_render_file(root_dir, "album.html", &context, NULL, NULL, 0);
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "[NONE][U:_fram/originals/b.jpg]") == 0);

cleanup:
  free(rendered_html);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// Comments are dropped from the rendered output.
static void test_comments_are_dropped(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "a{{! ignored }}b");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = template_render_file(root_dir, "album.html", &context, NULL, NULL, 0);
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "ab") == 0);

cleanup:
  free(rendered_html);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// The output bound belongs to the call. `template_render_file_limited` accepts output exactly at
// the bound it is given, reporting that length, and rejects one byte more, while
// `template_render_file` keeps the production bound and renders that same page.
static void test_output_bound_applies_per_call(void) {
  enum { OUTPUT_LEN_MAX = 64 * 1024 };
  char* padding = malloc((size_t)OUTPUT_LEN_MAX + 2);
  TEST_ASSERT(padding != NULL);
  if (padding == NULL) {
    return;
  }
  memset(padding, 'x', (size_t)OUTPUT_LEN_MAX + 1);
  padding[OUTPUT_LEN_MAX + 1] = '\0';
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, padding);
  if (root_dir == NULL) {
    free(padding);
    return;
  }
  padding[OUTPUT_LEN_MAX] = '\0';

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  size_t rendered_len = 0;
  char* rendered_html = NULL;
  char expected_err[ERROR_MESSAGE_SIZE];
  const int expected_err_len =
      snprintf(expected_err, sizeof(expected_err),
               "render exceeds max rendered output (%d bytes) at %d bytes (in 'album.html')",
               OUTPUT_LEN_MAX, OUTPUT_LEN_MAX + 1);
  if (!TEST_CHECK(write_fixture_file(root_dir, "at.html", padding) == 0)) {
    goto cleanup;
  }
  rendered_html = template_render_file_limited(root_dir, "at.html", &context, OUTPUT_LEN_MAX,
                                               &rendered_len, err, sizeof(err));
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strlen(rendered_html) == (size_t)OUTPUT_LEN_MAX);
  TEST_CHECK(rendered_len == (size_t)OUTPUT_LEN_MAX);
  free(rendered_html);

  TEST_CHECK(template_render_file_limited(root_dir, "album.html", &context, OUTPUT_LEN_MAX, NULL,
                                          err, sizeof(err)) == NULL);
  TEST_CHECK(expected_err_len > 0 && (size_t)expected_err_len < sizeof(expected_err));
  TEST_CHECK(strcmp(err, expected_err) == 0);

  rendered_html = template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err));
  if (!TEST_CHECK(rendered_html != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strlen(rendered_html) == (size_t)OUTPUT_LEN_MAX + 1);

cleanup:
  free(rendered_html);
  free(padding);
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// A template name that could escape the root is rejected, naming the offending template. The first
// call passes a real `err` buffer and asserts the whole message. The calls below pass `NULL, 0` and
// assert only the `NULL` return. That is deliberate rather than an oversight. The message is pinned
// once here, and repeating it per variant would couple every rejection variant to its wording.
static void test_rejects_unsafe_template_name(void) {
  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  const char* templates_dir = "tests/fixtures/gallery/templates";
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  char* rendered_html =
      template_render_file(templates_dir, "../album.html", &context, NULL, err, sizeof(err));
  TEST_CHECK(rendered_html == NULL);
  TEST_CHECK(strcmp(err, "template must be a safe relative template name: '../album.html'") == 0);
  rendered_html = template_render_file(templates_dir, "/etc/passwd", &context, NULL, NULL, 0);
  TEST_CHECK(rendered_html == NULL);
  // An embedded `..` segment is rejected by the safety check itself (a distinct path from a leading
  // `..`), independent of whether the target exists.
  rendered_html =
      template_render_file(templates_dir, "sub/../../secret.html", &context, NULL, NULL, 0);
  TEST_CHECK(rendered_html == NULL);
  gallery_config_free(&gallery_config);
}

// A partial name that could escape the partials directory is rejected, with a diagnostic. A name
// containing `/` never reaches the provider: mustache4c's own tag-name validation rejects it while
// compiling, so the failure surfaces as a compile error for the enclosing template.
static void test_rejects_unsafe_partial_name(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "{{> ../secret}}\n");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  char* rendered_html =
      template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err));
  TEST_CHECK(rendered_html == NULL);
  // Rejected by mustache4c's own tag validation, so the leading `%s` is its text and only the
  // position wrapper is this project's. Composed through the same format string as
  // `test_rejects_unclosed_section`, which is the other assertion of that wrapper.
  char expected_invalid_tag[ERROR_MESSAGE_SIZE];
  int n =
      snprintf(expected_invalid_tag, sizeof(expected_invalid_tag),
               "%s at line %u, column %u (in '%s')", "tag name is invalid", 1U, 1U, "album.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected_invalid_tag));
  TEST_CHECK(strcmp(err, expected_invalid_tag) == 0);

  // A name Mustache accepts but that is not a safe identifier is rejected by the provider, which
  // names the offending partial.
  if (!TEST_CHECK(write_fixture_file(root_dir, "album.html", "{{>nested.name}}\n") == 0)) {
    goto cleanup;
  }
  err[0] = '\0';
  rendered_html = template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err));
  TEST_CHECK(rendered_html == NULL);
  char expected_unsafe_name[ERROR_MESSAGE_SIZE];
  n = snprintf(expected_unsafe_name, sizeof(expected_unsafe_name),
               "partial name must contain only letters, digits, '_' and '-': '%s'", "nested.name");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected_unsafe_name));
  TEST_CHECK(strcmp(err, expected_unsafe_name) == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// A partial name too long for the path buffer names that limit, instead of reporting the same
// generic failure as an allocation error. The name is a legal identifier, so nothing rejects it
// before the path is built.
static void test_rejects_oversize_partial_name(void) {
  // `PARTIAL_PATH_SIZE` is file-local to `template.c`, so it is spelled out here. Changing it there
  // must update this. A name of exactly this length is the first that `partials/<name>.html` cannot
  // hold, one byte past the longest that fits.
  enum { PARTIAL_PATH_SIZE = 256 };
  const size_t name_len = PARTIAL_PATH_SIZE - (sizeof("partials/") - 1) - (sizeof(".html") - 1);
  char name[PARTIAL_PATH_SIZE];
  memset(name, 'a', name_len);
  name[name_len] = '\0';
  char source[PARTIAL_PATH_SIZE + 32];
  const int n = snprintf(source, sizeof(source), "{{>%s}}\n", name);
  TEST_ASSERT(n > 0 && (size_t)n < sizeof(source));

  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, source);
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err)) ==
             NULL);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(
      expected, sizeof(expected),
      "partial path exceeds max partial path length (%zu bytes) at %zu bytes: '%s'",
      (size_t)PARTIAL_PATH_SIZE - 1, strlen("partials/") + strlen(name) + strlen(".html"), name);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// An unreadable partial fails the render and reports the cause. It does not publish a page with a
// missing partial. mustache4c interprets a `NULL` callback result as an absent partial and reports
// success. Only the provider's failure flag converts this result to a failed render. A typo in
// `{{>card}}` is a typical cause.
static void test_rejects_unreadable_partial(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  // No `partials/card.html` is written, so resolving the reference fails on the read.
  const char* root_dir = write_template_fixture(root_dir_template, "before {{>card}} after");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err)) ==
             NULL);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(
      expected, sizeof(expected), "failed to read partial: %s ('%s/%s')",
      error_system_message(reason, sizeof(reason), ENOENT), root_dir, "partials/card.html");
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// A template holding a `NUL` byte is rejected before compiling, naming the template file. Rendering
// past it would emit a page with a raw `NUL` in it.
static void test_rejects_template_with_nul_byte(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  static const char bytes[] = {'a', '\0', 'b'};

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(
      expected, sizeof(expected),
      "failed to read template: contains an embedded NUL byte ('%s/album.html')", root_dir);
  if (!TEST_CHECK(write_fixture_bytes(root_dir, "album.html", bytes, sizeof(bytes)) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err)) ==
             NULL);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// A partial holding a `NUL` byte is rejected on the same grounds as a template, naming the partial
// file rather than the template that included it.
static void test_rejects_partial_with_nul_byte(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "before {{>card}} after");
  if (root_dir == NULL) {
    return;
  }
  static const char bytes[] = {'a', '\0', 'b'};

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(
      expected, sizeof(expected),
      "failed to read partial: contains an embedded NUL byte ('%s/partials/card.html')", root_dir);
  if (!TEST_CHECK(write_fixture_bytes(root_dir, "partials/card.html", bytes, sizeof(bytes)) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err)) ==
             NULL);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// A template or partial larger than `TEMPLATE_FILE_LEN_MAX` fails the render at the read, from its
// size, naming the limit and the size. The constant is file-local to `template.c`, so the 4 MiB
// below is spelled out and must change with it. The files are sparse, so the fixture costs no disk.
static void test_rejects_oversize_template_and_partial(void) {
  enum { TEMPLATE_FILE_LEN_MAX = 4 * 1024 * 1024 };
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "before {{>big}} after");
  if (root_dir == NULL) {
    return;
  }
  struct Arena arena;
  arena_init(&arena);
  const off_t file_len = (off_t)TEMPLATE_FILE_LEN_MAX + 1;
  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(write_fixture_file(root_dir, "big.html", "") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/big.html", "") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(truncate(path_join(root_dir, "big.html", &arena), file_len) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(truncate(path_join(root_dir, "partials/big.html", &arena), file_len) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(template_render_file(root_dir, "big.html", &context, NULL, err, sizeof(err)) == NULL);
  expected_len = snprintf(expected, sizeof(expected),
                          "failed to read template: exceeds max file size (%d bytes) at %jd bytes "
                          "('%s/big.html')",
                          TEMPLATE_FILE_LEN_MAX, (intmax_t)file_len, root_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

  err[0] = '\0';
  TEST_CHECK(template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err)) ==
             NULL);
  expected_len = snprintf(expected, sizeof(expected),
                          "failed to read partial: exceeds max file size (%d bytes) at %jd bytes "
                          "('%s/partials/big.html')",
                          TEMPLATE_FILE_LEN_MAX, (intmax_t)file_len, root_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  gallery_config_free(&gallery_config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// One distinct partial past the limit is rejected, naming the limit and the partial that reached
// it. `test_repeated_partial_stays_under_distinct_limit` covers the cache-hit side, where
// references beyond the limit cost nothing. This covers the miss side, where each distinct name
// consumes a slot. The guard is the only thing between a 65th distinct partial and a write past two
// fixed 64-element arrays living in `template_render_file`'s stack frame, so both sides of the
// comparison are asserted: exactly the limit renders, one more fails.
//
// `RENDER_PARTIAL_COUNT_MAX` is file-local to `template.c`, so it is spelled out here. Changing it
// there must update this.
static void test_rejects_partial_count_past_limit(void) {
  enum { RENDER_PARTIAL_COUNT_MAX = 64 };
  // `album.html` references exactly the limit. `over.html` references one more.
  char at_limit[RENDER_PARTIAL_COUNT_MAX * 10];
  char over_limit[(RENDER_PARTIAL_COUNT_MAX + 1) * 10];
  size_t at_limit_len = 0;
  size_t over_limit_len = 0;
  for (size_t i = 0; i <= (size_t)RENDER_PARTIAL_COUNT_MAX; i++) {
    char reference[16];
    const int n = snprintf(reference, sizeof(reference), "{{>p%zu}}", i);
    TEST_ASSERT(n > 0 && (size_t)n < sizeof(reference));
    if (i < (size_t)RENDER_PARTIAL_COUNT_MAX) {
      memcpy(at_limit + at_limit_len, reference, (size_t)n);
      at_limit_len += (size_t)n;
    }
    memcpy(over_limit + over_limit_len, reference, (size_t)n);
    over_limit_len += (size_t)n;
  }
  at_limit[at_limit_len] = '\0';
  over_limit[over_limit_len] = '\0';

  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char at_err[ERROR_MESSAGE_SIZE] = "";
  char* at_rendered = NULL;
  char over_err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "render exceeds max distinct partials (%d) while resolving partial 'p%d' "
               "(in 'over.html')",
               RENDER_PARTIAL_COUNT_MAX, RENDER_PARTIAL_COUNT_MAX);
  // One partial file per distinct name, plus the one that overflows the budget.
  for (size_t i = 0; i <= (size_t)RENDER_PARTIAL_COUNT_MAX; i++) {
    char partial_name[32];
    const int partial_name_len =
        snprintf(partial_name, sizeof(partial_name), "partials/p%zu.html", i);
    if (!TEST_CHECK(partial_name_len > 0 && (size_t)partial_name_len < sizeof(partial_name))) {
      goto cleanup;
    }
    if (!TEST_CHECK(write_fixture_file(root_dir, partial_name, "x") == 0)) {
      goto cleanup;
    }
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "album.html", at_limit) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "over.html", over_limit) == 0)) {
    goto cleanup;
  }

  at_rendered =
      template_render_file(root_dir, "album.html", &context, NULL, at_err, sizeof(at_err));
  TEST_CHECK(at_rendered != NULL);
  TEST_CHECK(at_err[0] == '\0');
  free(at_rendered);

  TEST_CHECK(template_render_file(root_dir, "over.html", &context, NULL, over_err,
                                  sizeof(over_err)) == NULL);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(over_err, expected) == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// A partial that includes itself is bounded by the expansion count rather than recursing until the
// stack runs out. The diagnostic names the limit and the likely cause.
//
// Unlike the output bound, this test uses the real limit. Reaching it proves that the selected
// constant has an acceptable cost. A full 300,000 expansions of this bare `{{>loop}}` uses
// single-digit MB and finishes in a fraction of a second. A cheaper override would test the
// mechanism without testing the number. This template resolves no names, so it measures the floor
// rather than the ceiling. A runaway partial referencing names costs a `struct Node` per name per
// expansion, which is the term `template.c`'s limits comment sizes the constant against.
static void test_rejects_recursive_partial(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "{{>loop}}");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/loop.html", "{{>loop}}") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err)) ==
             NULL);
  // `RENDER_EXPANSION_COUNT_MAX` is file-local to `template.c`, so it is spelled out here. Changing
  // it there must update this.
  TEST_CHECK(strcmp(err,
                    "render exceeds max partial expansions (300000); check for a partial that "
                    "includes itself (in 'album.html')") == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// Two partials that include each other are bounded by the same expansion count, so the bound is not
// specific to direct self-inclusion.
static void test_rejects_mutual_partial_cycle(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "{{>ping}}");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/ping.html", "{{>pong}}") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/pong.html", "{{>ping}}") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err)) ==
             NULL);
  // `RENDER_EXPANSION_COUNT_MAX` is spelled out here for the same reason as in
  // `test_rejects_recursive_partial`. Changing it in `template.c` must update this.
  TEST_CHECK(strcmp(err,
                    "render exceeds max partial expansions (300000); check for a partial that "
                    "includes itself (in 'album.html')") == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// An oversize render is rejected once the accumulated output passes the byte bound it was given.
//
// Reaching a byte-count limit means accumulating that many bytes. The production bound is sized for
// the largest real album, so asserting it at that value would cost most of a gigabyte of peak RSS
// for one diagnostic. This case passes a 64 KiB bound to `template_render_file_limited` instead.
static void test_rejects_oversize_render_output(void) {
  enum { OUTPUT_LEN_MAX = 64 * 1024 };

  // One kilobyte per expansion, so the bound is passed in far fewer expansions than the expansion
  // limit allows and this case lands on the output limit.
  enum { PADDING_LEN = 1024 };
  char partial[PADDING_LEN + sizeof("{{>card}}")];
  memset(partial, 'x', (size_t)PADDING_LEN);
  memcpy(partial + PADDING_LEN, "{{>card}}", sizeof("{{>card}}"));

  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "{{>card}}");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  // The measured length is whichever append first crossed the bound, so it depends on the chunking
  // mustache4c happens to use. The limit clause and the template attribution are deterministic and
  // are asserted instead.
  char expected_head[ERROR_MESSAGE_SIZE];
  const int expected_head_len =
      snprintf(expected_head, sizeof(expected_head),
               "render exceeds max rendered output (%d bytes) at ", OUTPUT_LEN_MAX);
  size_t err_actual_len = 0;
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", partial) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(template_render_file_limited(root_dir, "album.html", &context, OUTPUT_LEN_MAX, NULL,
                                          err, sizeof(err)) == NULL);
  TEST_CHECK(expected_head_len > 0 && (size_t)expected_head_len < sizeof(expected_head));
  TEST_CHECK(strncmp(err, expected_head, (size_t)expected_head_len) == 0);
  // Anchored at the end rather than searched for: the head above pins the start and the measured
  // length sits between them, so a floating `strstr` would leave anything trailing the message
  // undetected.
  static const char expected_tail[] = " bytes (in 'album.html')";
  err_actual_len = strlen(err);
  TEST_CHECK(err_actual_len >= sizeof(expected_tail) - 1 &&
             strcmp(err + err_actual_len - (sizeof(expected_tail) - 1), expected_tail) == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// Compilation rejects an unclosed section. The diagnostic gives the line and column of the
// incorrect tag, not only the template name. mustache4c reports this position through its parser
// callback. The section opens on line 3, so a hardcoded line 1 cannot pass.
static void test_rejects_unclosed_section(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir =
      write_template_fixture(root_dir_template, "first\nsecond\n{{#media}}{{title}}\n");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err)) ==
             NULL);
  // The leading `%s` is mustache4c's own parser text. Only the position and template around it are
  // this project's wording. Composing through the same format string keeps the two distinguishable
  // and keeps that wording greppable from `template.c`.
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
               "section-opening tag has no closer", 3U, 1U, "album.html");
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// A mismatched section closing tag fails to compile. The diagnostic says the closer does not match
// its opener rather than reporting a generic compile failure. The leading `%s` is mustache4c's own
// parser text. Composing through the same format string as `test_rejects_unclosed_section` keeps
// that wording distinguishable from this project's position wrapper. The closer sits at column 20,
// so a hardcoded 1 cannot pass.
static void test_rejects_section_name_mismatch(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "{{#media}}{{title}}{{/wrong}}");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err)) ==
             NULL);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
               "name of section-closing tag does not match corresponding section-opening "
               "tag",
               1U, 20U, "album.html");
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// Malformed tag syntax and unsupported block commands are each rejected with the library's own
// reason and the column of the offending tag, so one malformed shape cannot pass on another's
// failure. Every reason below is mustache4c's text and only the position wrapper is this project's,
// composed through the same format string as `test_rejects_unclosed_section`. Each source is a
// single line, so the line is 1 by construction and the column is what distinguishes the cases. An
// unbalanced triple brace fails here, while compiling, before `node_dump` could reject it.
static void test_rejects_malformed_tags(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};

  static const struct {
    const char* source;
    const char* reason;
    unsigned column;
  } cases[] = {
      {"{{title\n", "tag opener has no closer", 1U},
      {"{{{title}}\n", "tag closer is incompatible with its opener", 11U},
      {"{{/each}}\n", "section-closing tag has no opener", 1U},
      {"{{#if album}}\n", "tag name is invalid", 1U},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if (!TEST_CHECK(write_fixture_file(root_dir, "album.html", cases[i].source) == 0)) {
      goto cleanup;
    }
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err)) ==
               NULL);
    char expected[ERROR_MESSAGE_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')", cases[i].reason,
                 1U, cases[i].column, "album.html");
    if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
      goto cleanup;
    }
    TEST_CHECK(strcmp(err, expected) == 0);
  }

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// A syntax error inside a partial names the partial's *file*, not the bare tag name.
// `(in '<file>')` must denote a file, and `card` is not one. It is also ambiguous with a top-level
// template of the same name, which reaches the identical format string. The position is the
// partial's own, so line 2 here is line 2 of `partials/card.html`, not of the `album.html` that
// included it.
static void test_rejects_syntax_error_in_partial(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "{{>card}}\n");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
               "section-opening tag has no closer", 2U, 1U, "partials/card.html");
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html",
                                     "first\n{{#media}}{{title}}\n") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(template_render_file(root_dir, "album.html", &context, NULL, err, sizeof(err)) ==
             NULL);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

// Triple braces and the ampersand tag cannot bypass the single HTML-escaping boundary, and neither
// can a partial, whose rejection is attributed to the template being rendered.
static void test_rejects_unescaped_interpolation(void) {
  char root_dir_template[] = "/tmp/fram-template-XXXXXX";
  const char* root_dir = write_template_fixture(root_dir_template, "{{{gallery.title}}}");
  if (root_dir == NULL) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = "Root", .url_path = "index.html", .path_to_root = ""};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};

  static const struct {
    const char* template_name;
    const char* expected;
  } cases[] = {
      {"album.html", "unescaped Mustache interpolation is not supported (in 'album.html')"},
      {"ampersand.html", "unescaped Mustache interpolation is not supported (in 'ampersand.html')"},
      {"outer.html", "unescaped Mustache interpolation is not supported (in 'outer.html')"},
  };
  if (!TEST_CHECK(write_fixture_file(root_dir, "ampersand.html", "{{& gallery.title}}") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "outer.html", "{{>raw}}") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/raw.html", "{{{album.title}}}") == 0)) {
    goto cleanup;
  }
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char err[ERROR_MESSAGE_SIZE] = "";
    char* rendered_html =
        template_render_file(root_dir, cases[i].template_name, &context, NULL, err, sizeof(err));
    TEST_CHECK(rendered_html == NULL);
    TEST_CHECK(strcmp(err, cases[i].expected) == 0);
    TEST_MSG("template: %s, err: %s", cases[i].template_name, err);
  }

cleanup:
  gallery_config_free(&gallery_config);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"renders gallery tree with page-relative URLs",
     test_renders_gallery_tree_with_page_relative_urls},
    {"renders sub-album cover", test_renders_sub_album_cover},
    {"renders generator", test_renders_generator},
    {"renders partial", test_renders_partial},
    {"repeated partial renders same content each time",
     test_repeated_partial_renders_same_content_each_time},
    {"repeated partial stays under distinct limit",
     test_repeated_partial_stays_under_distinct_limit},
    {"empty template yields empty string", test_empty_template_yields_empty_string},
    {"unknown variable renders empty", test_unknown_variable_renders_empty},
    {"inverted section renders when list empty", test_inverted_section_renders_when_list_empty},
    {"empty field is falsey as section", test_empty_field_is_falsey_as_section},
    {"comments are dropped", test_comments_are_dropped},
    {"output bound applies per call", test_output_bound_applies_per_call},
    {"rejects unsafe template name", test_rejects_unsafe_template_name},
    {"rejects unsafe partial name", test_rejects_unsafe_partial_name},
    {"rejects oversize partial name", test_rejects_oversize_partial_name},
    {"rejects unreadable partial", test_rejects_unreadable_partial},
    {"rejects template with NUL byte", test_rejects_template_with_nul_byte},
    {"rejects partial with NUL byte", test_rejects_partial_with_nul_byte},
    {"rejects oversize template and partial", test_rejects_oversize_template_and_partial},
    {"rejects partial count past limit", test_rejects_partial_count_past_limit},
    {"rejects recursive partial", test_rejects_recursive_partial},
    {"rejects mutual partial cycle", test_rejects_mutual_partial_cycle},
    {"rejects oversize render output", test_rejects_oversize_render_output},
    {"rejects unclosed section", test_rejects_unclosed_section},
    {"rejects section name mismatch", test_rejects_section_name_mismatch},
    {"rejects malformed tags", test_rejects_malformed_tags},
    {"rejects syntax error in partial", test_rejects_syntax_error_in_partial},
    {"rejects unescaped interpolation", test_rejects_unescaped_interpolation},
    {NULL, NULL},
};
