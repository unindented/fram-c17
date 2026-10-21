#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * Size of a buffer for an expected diagnostic. The log never truncates, so this only has to hold
 * the longest message a test spells out.
 */
enum { EXPECTED_SIZE = 1024 };

/**
 * @brief Initializes shared gallery metadata for template tests.
 *
 * The assigned fields are string literals.
 *
 * @param gallery_config Configuration to initialize. Must not be `NULL`.
 */
static void init_test_gallery_config(struct GalleryConfig* gallery_config) {
  gallery_config_init(gallery_config);
  gallery_config->title = S("Gallery");
  gallery_config->author = S("Ada");
}

/**
 * @brief Creates a fixture root holding one `album.html` template.
 *
 * @param root_dir Writable `mkdtemp` template. Receives the created directory path. Must not be
 *                 `NULL`.
 * @param text     Terminated template text to write.
 * @return `0` on success, or `-1` after recording a test-plumbing failure, with no fixture left
 *         behind.
 */
static int write_template_fixture(char root_dir[static 1], const char* text) {
  if (init_fixture_dir(root_dir) != 0) {
    return -1;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "album.html", text) == 0)) {
    remove_fixture_tree(root_dir);
    return -1;
  }
  return 0;
}

/**
 * @brief Writes one fixture file whose bytes may include a `NUL`.
 *
 * @param root_dir      Fixture root directory.
 * @param relative_path Relative fixture path below `root_dir`.
 * @param data          Bytes to write.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
static int write_fixture_bytes(const char* root_dir, const char* relative_path, struct Str data) {
  char fixture_path[256];
  const int n = snprintf(fixture_path, sizeof(fixture_path), "%s/%s", root_dir, relative_path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(fixture_path));
  if (n <= 0 || (size_t)n >= sizeof(fixture_path)) {
    return -1;
  }
  struct Log log = test_log();
  const int rc = os_write(os_posix(), str_from_cstr(fixture_path), (struct OsSource){.bytes = data},
                          S("failed to write fixture"), &log);
  TEST_CHECK(rc == 0);
  return rc;
}

/**
 * @brief Renders through `template_render_file_limited` and hands back the HTML as a C string.
 *
 * This is a deliberate test-only bridge back to C strings. Every assertion below inspects rendered
 * text with C string functions such as `strcmp` and `strstr`, so the helper hands it back
 * terminated, copied after the HTML in `perm`. Rendered output holds no `NUL` byte, so the copy
 * loses nothing.
 *
 * @param templates_dir  Terminated template directory root.
 * @param template_name  Terminated template name.
 * @param context        Template context. Must not be `NULL`.
 * @param output_len_max Largest rendered output accepted, in bytes.
 * @param perm           Arena that owns the HTML and its terminated copy. Must not be `NULL`.
 * @param html_out       Receives the terminated HTML on success, or `NULL` when the test asserts
 *                       only the result.
 * @param log            Log that receives the failure, or `NULL` when the test asserts only the
 *                       result.
 * @return `0` on success, or `-1` when the render failed.
 */
static int render_limited(const char* templates_dir,
                          const char* template_name,
                          const struct TemplateContext* context,
                          ptrdiff_t output_len_max,
                          struct Arena* perm,
                          char** html_out,
                          struct Log* log) {
  struct Log discarded = {0};
  if (log == NULL) {
    discarded = test_log();
    log = &discarded;
  }
  struct Str html = {0};
  if (template_render_file_limited(str_from_cstr(templates_dir), str_from_cstr(template_name),
                                   context, output_len_max, os_posix(), perm, test_arena(), &html,
                                   log) != 0) {
    return -1;
  }
  if (html_out != NULL) {
    *html_out = str_cstr(html, perm);
  }
  return 0;
}

/**
 * @brief Renders through `template_render_file` with the production bound and hands back the HTML
 *        as a C string, the same test-only bridge as `render_limited`.
 *
 * @param templates_dir Terminated template directory root.
 * @param template_name Terminated template name.
 * @param context       Template context. Must not be `NULL`.
 * @param perm          Arena that owns the HTML and its terminated copy. Must not be `NULL`.
 * @param html_out      Receives the terminated HTML on success, or `NULL` when the test asserts
 *                      only the result.
 * @param log           Log that receives the failure, or `NULL` when the test asserts only the
 *                      result.
 * @return `0` on success, or `-1` when the render failed.
 */
static int render(const char* templates_dir,
                  const char* template_name,
                  const struct TemplateContext* context,
                  struct Arena* perm,
                  char** html_out,
                  struct Log* log) {
  struct Log discarded = {0};
  if (log == NULL) {
    discarded = test_log();
    log = &discarded;
  }
  struct Str html = {0};
  if (template_render_file(str_from_cstr(templates_dir), str_from_cstr(template_name), context,
                           os_posix(), perm, test_arena(), &html, log) != 0) {
    return -1;
  }
  if (html_out != NULL) {
    *html_out = str_cstr(html, perm);
  }
  return 0;
}

// Album, breadcrumb, repeated-media, boolean, escaping, and contextual URL nodes resolve together.
static void test_renders_gallery_tree_with_page_relative_urls(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  const char* text =
      "{{gallery.title}}|{{album.title}}|{{#breadcrumbs}}[{{title}}={{url}}]{{/breadcrumbs}}|"
      "{{#media}}{{title}}={{s.url}}/{{s.width}}x{{s.height}},"
      "{{card.url}}/{{card.width}}x{{card.height}},{{original_url}}{{#is_image}}I{{/is_image}};"
      "{{/media}}";
  if (write_template_fixture(root_dir, text) != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  gallery_config_init(&gallery_config);
  gallery_config.title = S("A & B");
  gallery_config.author = S("Ada");
  gallery_config.templates_dir = str_from_cstr(root_dir);

  struct Album root = {.source_dir = S(""),
                       .title = S("Root"),
                       .url_path = S("index.html"),
                       .path_to_root = S(""),
                       .depth = 0};
  struct MediaDerivative derivatives[] = {
      {.name = S("s"), .url = S("_fram/r/s/x.jpg"), .width_px = 120, .height_px = 120},
      {.name = S("card"), .url = S("_fram/r/card/x.jpg"), .width_px = 480, .height_px = 320},
      // A configured name cannot shadow a built-in media field.
      {.name = S("title"), .url = S("_fram/r/title/x.jpg"), .width_px = 1, .height_px = 1},
  };
  struct MediaItem media = {.title = S("x<y.jpg"),
                            .kind = MEDIA_KIND_IMAGE,
                            .derivatives = derivatives,
                            .derivative_count = 3,
                            .original_url = S("_fram/originals/x.jpg")};
  struct MediaItem* media_items[] = {&media};
  struct Album child = {.source_dir = S("trip"),
                        .title = S("Trip"),
                        .url_path = S("trip/index.html"),
                        .path_to_root = S("../"),
                        .media = media_items,
                        .media_count = 1,
                        .parent = &root,
                        .depth = 1};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &child};
  char* rendered_html = NULL;
  if (!TEST_CHECK(render(root_dir, "album.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html,
                    "A &amp; B|Trip|[Root=../index.html][Trip=../trip/index.html]|"
                    "x&lt;y.jpg=../_fram/r/s/x.jpg/120x120,"
                    "../_fram/r/card/x.jpg/480x320,"
                    "../_fram/originals/x.jpgI;") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// Sub-album links and inherited covers are materialized relative to the page being rendered.
static void test_renders_sub_album_cover(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir,
                             "{{#sub_albums}}{{title}}={{url}}={{cover.s.url}}={{cover.s.width}}x"
                             "{{cover.s.height}}={{item_count}};"
                             "{{/sub_albums}}") != 0) {
    return;
  }
  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct MediaDerivative cover_derivatives[] = {
      {.name = S("s"), .url = S("_fram/r/s/trip/x.jpg"), .width_px = 120, .height_px = 120},
  };
  struct MediaItem cover = {.derivatives = cover_derivatives, .derivative_count = 1};
  struct Album child = {.title = S("Trip"),
                        .url_path = S("trip/index.html"),
                        .cover = &cover,
                        .item_count_total = 3,
                        .depth = 1};
  struct Album* children[] = {&child};
  struct Album root = {.title = S("Root"),
                       .url_path = S("index.html"),
                       .path_to_root = S(""),
                       .sub_albums = children,
                       .sub_album_count = 1};
  child.parent = &root;
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = NULL;
  if (!TEST_CHECK(render(root_dir, "album.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "Trip=trip/index.html=_fram/r/s/trip/x.jpg=120x120=3;") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// `generator` names the tool and its version, at the root and inside a section.
static void test_renders_generator(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{generator}}|{{#media}}{{generator}}{{/media}}") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct MediaItem item = {0};
  struct MediaItem* media[] = {&item};
  struct Album root = {.title = S("Root"),
                       .url_path = S("index.html"),
                       .path_to_root = S(""),
                       .media = media,
                       .media_count = 1};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = NULL;
  const int render_rc = render(root_dir, "album.html", &context, &perm, &rendered_html, NULL);
  const struct Str version = fram_version_string();
  char expected[64];
  const int expected_len = snprintf(expected, sizeof(expected), "fram %.*s|fram %.*s",
                                    STR_FMT(version), STR_FMT(version));
  if (!TEST_CHECK(render_rc == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, expected) == 0);
  TEST_MSG("rendered: '%s'", rendered_html);

cleanup:
  remove_fixture_tree(root_dir);
}

// A safe partial loads and renders in the parent context, with its values still escaped.
static void test_renders_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "before {{> card}} after") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {
      .title = S("Root <One>"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = NULL;
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "{{album.title}}") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(render(root_dir, "album.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "before Root &lt;One&gt; after") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// A partial referenced twice per media item across three items expands six times and renders each
// item's own content each time. The compile cache is not observable through the API, so this pins
// what a broken cache would break, stale or empty expansions, rather than the number of compiles.
static void test_repeated_partial_renders_same_content_each_time(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{#media}}{{>card}}{{>card}}{{/media}}") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct MediaItem items[] = {{.title = S("p1")}, {.title = S("p2")}, {.title = S("p3")}};
  struct MediaItem* media[] = {&items[0], &items[1], &items[2]};
  struct Album root = {.title = S("Root"),
                       .url_path = S("index.html"),
                       .path_to_root = S(""),
                       .media = media,
                       .media_count = 3};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  char* rendered_html = NULL;
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "[{{title}}]") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(render(root_dir, "album.html", &context, &perm, &rendered_html, &log) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(strcmp(rendered_html, "[p1][p1][p2][p2][p3][p3]") == 0);

cleanup:
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
  struct Arena perm = test_arena();
  enum { RENDER_PARTIAL_COUNT_MAX = 64 };
  enum { REFERENCE_COUNT = RENDER_PARTIAL_COUNT_MAX + 6 };
  enum { REFERENCE_LEN = sizeof("{{>card}}") - 1 };
  enum { EXPANSION_LEN = sizeof("[Root]") - 1 };
  char source[REFERENCE_COUNT * REFERENCE_LEN + 1];
  char expected[REFERENCE_COUNT * EXPANSION_LEN + 1];
  for (ptrdiff_t i = 0; i < REFERENCE_COUNT; i++) {
    memcpy(source + i * REFERENCE_LEN, "{{>card}}", REFERENCE_LEN);
    memcpy(expected + i * EXPANSION_LEN, "[Root]", EXPANSION_LEN);
  }
  source[REFERENCE_COUNT * REFERENCE_LEN] = '\0';
  expected[REFERENCE_COUNT * EXPANSION_LEN] = '\0';

  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, source) != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  char* rendered_html = NULL;
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "[{{album.title}}]") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(render(root_dir, "album.html", &context, &perm, &rendered_html, &log) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(strcmp(rendered_html, expected) == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// An empty template renders as empty output.
static void test_empty_template_yields_empty_string(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = NULL;
  if (!TEST_CHECK(render(root_dir, "album.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// An unknown variable renders as empty text per the Mustache spec, at the root and in a subtree.
static void test_unknown_variable_renders_empty(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "[{{typo}}|{{gallery.typo}}]\n") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = NULL;
  if (!TEST_CHECK(render(root_dir, "album.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "[|]\n") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// An inverted section renders only when the list is empty.
static void test_inverted_section_renders_when_list_empty(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{#media}}x{{/media}}{{^media}}none{{/media}}") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);

  struct Album empty = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context_empty = {.gallery_config = &gallery_config,
                                                .album_current = &empty};
  struct MediaItem item = {.title = S("x.jpg")};
  struct MediaItem* media[] = {&item};
  struct Album full = {.title = S("Root"),
                       .url_path = S("index.html"),
                       .path_to_root = S(""),
                       .media = media,
                       .media_count = 1};
  const struct TemplateContext context_full = {.gallery_config = &gallery_config,
                                               .album_current = &full};
  char* rendered_html = NULL;
  if (!TEST_CHECK(render(root_dir, "album.html", &context_empty, &perm, &rendered_html, NULL) ==
                  0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "none") == 0);

  if (!TEST_CHECK(render(root_dir, "album.html", &context_full, &perm, &rendered_html, NULL) ==
                  0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "x") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// An empty field value is falsey as a section, so an unset field reads as absent. Were an empty
// scalar truthy, `{{#original_url}}` would render for every item and `{{^original_url}}` for none,
// so both branches are asserted here. The field is media-only, so an empty value cannot resolve
// through an outer context instead, as an empty media `title` would to the album's `title`.
// `gallery.base_url` is empty unless configured, so a template guarding it relies on this rule.
static void test_empty_field_is_falsey_as_section(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir,
                             "{{#media}}[{{#original_url}}U:{{original_url}}{{/original_url}}"
                             "{{^original_url}}NONE{{/original_url}}]{{/media}}") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct MediaItem first = {.original_url = S("")};
  struct MediaItem second = {.original_url = S("_fram/originals/b.jpg")};
  struct MediaItem* media[] = {&first, &second};
  struct Album root = {.title = S("Root"),
                       .url_path = S("index.html"),
                       .path_to_root = S(""),
                       .media = media,
                       .media_count = 2};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = NULL;
  if (!TEST_CHECK(render(root_dir, "album.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "[NONE][U:_fram/originals/b.jpg]") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// Comments are dropped from the rendered output.
static void test_comments_are_dropped(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "a{{! ignored }}b") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  char* rendered_html = NULL;
  if (!TEST_CHECK(render(root_dir, "album.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "ab") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// The output bound belongs to the call. `template_render_file_limited` accepts output exactly at
// the bound it is given, reporting that length, and rejects one byte more, while
// `template_render_file` keeps the production bound and renders that same page.
static void test_output_bound_applies_per_call(void) {
  struct Arena perm = test_arena();
  enum { OUTPUT_LEN_MAX = 64 * 1024 };
  char* padding = arena_new(&perm, OUTPUT_LEN_MAX + 2, char);
  memset(padding, 'x', (size_t)OUTPUT_LEN_MAX + 1);
  padding[OUTPUT_LEN_MAX + 1] = '\0';
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, padding) != 0) {
    return;
  }
  padding[OUTPUT_LEN_MAX] = '\0';

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  char* rendered_html = NULL;
  char expected_err[EXPECTED_SIZE];
  const int expected_err_len =
      snprintf(expected_err, sizeof(expected_err),
               "render exceeds max rendered output (%d bytes) at %d bytes (in 'album.html')",
               OUTPUT_LEN_MAX, OUTPUT_LEN_MAX + 1);
  if (!TEST_CHECK(write_fixture_file(root_dir, "at.html", padding) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(render_limited(root_dir, "at.html", &context, OUTPUT_LEN_MAX, &perm,
                                 &rendered_html, &log) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strlen(rendered_html) == (size_t)OUTPUT_LEN_MAX);

  TEST_CHECK(render_limited(root_dir, "album.html", &context, OUTPUT_LEN_MAX, &perm, NULL, &log) ==
             -1);
  TEST_CHECK(expected_err_len > 0 && (size_t)expected_err_len < sizeof(expected_err));
  TEST_CHECK(test_log_is_one(&log, expected_err));

  if (!TEST_CHECK(render(root_dir, "album.html", &context, &perm, &rendered_html, &log) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strlen(rendered_html) == (size_t)OUTPUT_LEN_MAX + 1);

cleanup:
  remove_fixture_tree(root_dir);
}

// A partial name that could escape the partials directory is rejected, with a diagnostic. A name
// holding `..` never reaches the provider: mustache4c's own tag-name validation rejects it while
// compiling, so the failure surfaces as a compile error for the enclosing template.
static void test_rejects_unsafe_partial_name(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{> ../secret}}\n") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  // Rejected by mustache4c's own tag validation, so the leading `%s` is its text and only the
  // position wrapper is this project's. Composed through the same format string as
  // `test_rejects_unclosed_section`, which is the other assertion of that wrapper.
  char expected_invalid_tag[EXPECTED_SIZE];
  int n =
      snprintf(expected_invalid_tag, sizeof(expected_invalid_tag),
               "%s at line %u, column %u (in '%s')", "tag name is invalid", 1U, 1U, "album.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected_invalid_tag));
  TEST_CHECK(test_log_is_one(&log, expected_invalid_tag));

  // A name Mustache accepts but that is not a safe identifier is rejected by the provider, which
  // names the offending partial and the template that references it.
  if (!TEST_CHECK(write_fixture_file(root_dir, "album.html", "{{>nested.name}}\n") == 0)) {
    goto cleanup;
  }
  log = test_log();
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  char expected_unsafe_name[EXPECTED_SIZE];
  n = snprintf(
      expected_unsafe_name, sizeof(expected_unsafe_name),
      "partial name must contain only letters, digits, '_' and '-': '%s' (in 'album.html')",
      "nested.name");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected_unsafe_name));
  TEST_CHECK(test_log_is_one(&log, expected_unsafe_name));

cleanup:
  remove_fixture_tree(root_dir);
}

// A bad partial reference names the file it is written in. Inside a partial that is the partial,
// not the template including it. mustache4c resolves the reference while it renders and says
// nothing when a partial ends, so the second case puts a reference in the template after a partial
// has finished, where the template holds it again.
static void test_rejects_unsafe_partial_name_in_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>card}}\n") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "outer {{>a/b}}\n") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  TEST_CHECK(test_log_is_one(&log,
                             "partial name must contain only letters, digits, '_' and '-': 'a/b' "
                             "(in 'partials/card.html')"));

  if (!TEST_CHECK(write_fixture_file(root_dir, "album.html", "{{>card}}\n{{>a/b}}\n") == 0) ||
      !TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "card\n") == 0)) {
    goto cleanup;
  }
  log = test_log();
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  TEST_CHECK(test_log_is_one(
      &log,
      "partial name must contain only letters, digits, '_' and '-': 'a/b' (in 'album.html')"));

cleanup:
  remove_fixture_tree(root_dir);
}

// An unreadable partial fails the render and reports the cause. It does not publish a page with a
// missing partial. mustache4c interprets a `NULL` callback result as an absent partial and reports
// success. Only the provider's failure flag converts this result to a failed render. A typo in
// `{{>card}}` is a typical cause.
static void test_rejects_unreadable_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  // No `partials/card.html` is written, so resolving the reference fails on the read.
  if (write_template_fixture(root_dir, "before {{>card}} after") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  char expected[EXPECTED_SIZE];
  const int expected_len = snprintf(
      expected, sizeof(expected), "failed to read partial: %s ('%s/%s')",
      error_system_message(reason, sizeof(reason), ENOENT), root_dir, "partials/card.html");
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A template holding a `NUL` byte is rejected before compiling, naming the template file. Rendering
// past it would emit a page with a raw `NUL` in it.
static void test_rejects_template_with_nul_byte(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  static const char bytes[] = {'a', '\0', 'b'};

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  const int expected_len = snprintf(
      expected, sizeof(expected),
      "failed to read template: contains an embedded NUL byte ('%s/album.html')", root_dir);
  if (!TEST_CHECK(write_fixture_bytes(root_dir, "album.html", (struct Str){bytes, sizeof(bytes)}) ==
                  0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A partial holding a `NUL` byte is rejected on the same grounds as a template, naming the partial
// file rather than the template that included it.
static void test_rejects_partial_with_nul_byte(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "before {{>card}} after") != 0) {
    return;
  }
  static const char bytes[] = {'a', '\0', 'b'};

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  const int expected_len = snprintf(
      expected, sizeof(expected),
      "failed to read partial: contains an embedded NUL byte ('%s/partials/card.html')", root_dir);
  if (!TEST_CHECK(write_fixture_bytes(root_dir, "partials/card.html",
                                      (struct Str){bytes, sizeof(bytes)}) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A template or partial larger than `TEMPLATE_FILE_LEN_MAX` fails the render at the read, from its
// size, naming the limit and the size. The files are sparse, so the fixture costs no disk.
static void test_rejects_oversize_template_and_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "before {{>big}} after") != 0) {
    return;
  }
  struct Arena arena = test_arena();
  const off_t file_len = (off_t)TEMPLATE_FILE_LEN_MAX + 1;
  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(write_fixture_file(root_dir, "big.html", "") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/big.html", "") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(
          truncate(str_cstr(path_join(str_from_cstr(root_dir), S("big.html"), &arena), &arena),
                   file_len) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(
          truncate(
              str_cstr(path_join(str_from_cstr(root_dir), S("partials/big.html"), &arena), &arena),
              file_len) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(render(root_dir, "big.html", &context, &perm, NULL, &log) == -1);
  expected_len = snprintf(expected, sizeof(expected),
                          "failed to read template: exceeds max file size (%d bytes) at %jd bytes "
                          "('%s/big.html')",
                          TEMPLATE_FILE_LEN_MAX, (intmax_t)file_len, root_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_one(&log, expected));

  log = test_log();
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  expected_len = snprintf(expected, sizeof(expected),
                          "failed to read partial: exceeds max file size (%d bytes) at %jd bytes "
                          "('%s/partials/big.html')",
                          TEMPLATE_FILE_LEN_MAX, (intmax_t)file_len, root_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
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
  struct Arena perm = test_arena();
  enum { RENDER_PARTIAL_COUNT_MAX = 64 };
  // `album.html` references exactly the limit. `over.html` references one more.
  char at_limit[RENDER_PARTIAL_COUNT_MAX * 10];
  char over_limit[(RENDER_PARTIAL_COUNT_MAX + 1) * 10];
  size_t at_limit_len = 0;
  size_t over_limit_len = 0;
  for (ptrdiff_t i = 0; i <= RENDER_PARTIAL_COUNT_MAX; i++) {
    char reference[16];
    const int n = snprintf(reference, sizeof(reference), "{{>p%td}}", i);
    TEST_ASSERT(n > 0 && (size_t)n < sizeof(reference));
    if (i < RENDER_PARTIAL_COUNT_MAX) {
      memcpy(at_limit + at_limit_len, reference, (size_t)n);
      at_limit_len += (size_t)n;
    }
    memcpy(over_limit + over_limit_len, reference, (size_t)n);
    over_limit_len += (size_t)n;
  }
  at_limit[at_limit_len] = '\0';
  over_limit[over_limit_len] = '\0';

  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log at_log = test_log();
  char* at_rendered = NULL;
  // Every partial renders as one `x`.
  char at_expected[RENDER_PARTIAL_COUNT_MAX + 1];
  struct Log over_log = test_log();
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "render exceeds max distinct partials (%d) while resolving partial 'p%d' "
               "(in 'over.html')",
               RENDER_PARTIAL_COUNT_MAX, RENDER_PARTIAL_COUNT_MAX);
  memset(at_expected, 'x', RENDER_PARTIAL_COUNT_MAX);
  at_expected[RENDER_PARTIAL_COUNT_MAX] = '\0';
  // One partial file per distinct name, plus the one that overflows the budget.
  for (ptrdiff_t i = 0; i <= RENDER_PARTIAL_COUNT_MAX; i++) {
    char partial_name[32];
    const int partial_name_len =
        snprintf(partial_name, sizeof(partial_name), "partials/p%td.html", i);
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

  if (!TEST_CHECK(render(root_dir, "album.html", &context, &perm, &at_rendered, &at_log) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_empty(&at_log));
  TEST_CHECK(strcmp(at_rendered, at_expected) == 0);

  TEST_CHECK(render(root_dir, "over.html", &context, &perm, NULL, &over_log) == -1);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_one(&over_log, expected));

cleanup:
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
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>loop}}") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/loop.html", "{{>loop}}") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  // `RENDER_EXPANSION_COUNT_MAX` is file-local to `template.c`, so it is spelled out here. Changing
  // it there must update this.
  TEST_CHECK(
      test_log_is_one(&log,
                      "render exceeds max partial expansions (300000); check for a partial that "
                      "includes itself (in 'album.html')"));

cleanup:
  remove_fixture_tree(root_dir);
}

// Two partials that include each other are bounded by the same expansion count, so the bound is not
// specific to direct self-inclusion.
static void test_rejects_mutual_partial_cycle(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>ping}}") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/ping.html", "{{>pong}}") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/pong.html", "{{>ping}}") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  // `RENDER_EXPANSION_COUNT_MAX` is spelled out here for the same reason as in
  // `test_rejects_recursive_partial`. Changing it in `template.c` must update this.
  TEST_CHECK(
      test_log_is_one(&log,
                      "render exceeds max partial expansions (300000); check for a partial that "
                      "includes itself (in 'album.html')"));

cleanup:
  remove_fixture_tree(root_dir);
}

// An oversize render is rejected once the accumulated output passes the byte bound it was given.
//
// Reaching a byte-count limit means accumulating that many bytes. The production bound is sized for
// the largest real album, so asserting it at that value would cost most of a gigabyte of peak RSS
// for one diagnostic. This case passes a 64 KiB bound to `template_render_file_limited` instead.
static void test_rejects_oversize_render_output(void) {
  struct Arena perm = test_arena();
  enum { OUTPUT_LEN_MAX = 64 * 1024 };

  // One kilobyte per expansion, so the bound is passed in far fewer expansions than the expansion
  // limit allows and this case lands on the output limit.
  enum { PADDING_LEN = 1024 };
  char partial[PADDING_LEN + sizeof("{{>card}}")];
  memset(partial, 'x', (size_t)PADDING_LEN);
  memcpy(partial + PADDING_LEN, "{{>card}}", sizeof("{{>card}}"));

  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>card}}") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  // The measured length is whichever append first crossed the bound, so it depends on the chunking
  // mustache4c happens to use. The limit clause and the template attribution are deterministic and
  // are asserted instead.
  char expected_head[EXPECTED_SIZE];
  const int expected_head_len =
      snprintf(expected_head, sizeof(expected_head),
               "render exceeds max rendered output (%d bytes) at ", OUTPUT_LEN_MAX);
  struct Str failure = {0};
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", partial) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render_limited(root_dir, "album.html", &context, OUTPUT_LEN_MAX, &perm, NULL, &log) ==
             -1);
  TEST_CHECK(expected_head_len > 0 && (size_t)expected_head_len < sizeof(expected_head));
  failure = buf_str(&log.buf);
  TEST_CHECK(str_startswith(failure, (struct Str){expected_head, expected_head_len}));
  // Anchored at the end rather than searched for: the head above pins the start and the measured
  // length sits between them, so a floating `strstr` would leave anything trailing the message
  // undetected.
  static const char expected_tail[] = " bytes (in 'album.html')\n";
  TEST_CHECK(
      str_endswith(failure, (struct Str){expected_tail, (ptrdiff_t)sizeof(expected_tail) - 1}));
  TEST_CHECK(log.errors == 1);
  TEST_MSG("actual: '%.*s'", STR_FMT(failure));

cleanup:
  remove_fixture_tree(root_dir);
}

// Compilation rejects an unclosed section. The diagnostic gives the line and column of the
// incorrect tag, not only the template name. mustache4c reports this position through its parser
// callback. The section opens on line 3, so a hardcoded line 1 cannot pass.
static void test_rejects_unclosed_section(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "first\nsecond\n{{#media}}{{title}}\n") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  // The leading `%s` is mustache4c's own parser text. Only the position and template around it are
  // this project's wording. Composing through the same format string keeps the two distinguishable
  // and keeps that wording greppable from `template.c`.
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
               "section-opening tag has no closer", 3U, 1U, "album.html");
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A mismatched section closing tag fails to compile. The diagnostic says the closer does not match
// its opener rather than reporting a generic compile failure. The leading `%s` is mustache4c's own
// parser text. Composing through the same format string as `test_rejects_unclosed_section` keeps
// that wording distinguishable from this project's position wrapper. The closer sits at column 20,
// so a hardcoded 1 cannot pass.
static void test_rejects_section_name_mismatch(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{#media}}{{title}}{{/wrong}}") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
               "name of section-closing tag does not match corresponding section-opening "
               "tag",
               1U, 20U, "album.html");
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// Malformed tag syntax and unsupported block commands are each rejected with the library's own
// reason and the column of the offending tag, so one malformed shape cannot pass on another's
// failure. Every reason below is mustache4c's text and only the position wrapper is this project's,
// composed through the same format string as `test_rejects_unclosed_section`. Each source is a
// single line, so the line is 1 by construction and the column is what distinguishes the cases. An
// unbalanced triple brace fails here, while compiling, before `node_dump` could reject it.
static void test_rejects_malformed_tags(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
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
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    if (!TEST_CHECK(write_fixture_file(root_dir, "album.html", cases[i].source) == 0)) {
      goto cleanup;
    }
    struct Log log = test_log();
    TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
    char expected[EXPECTED_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')", cases[i].reason,
                 1U, cases[i].column, "album.html");
    if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
      goto cleanup;
    }
    TEST_CHECK(test_log_is_one(&log, expected));
  }

cleanup:
  remove_fixture_tree(root_dir);
}

// A syntax error inside a partial names the partial's *file*, not the bare tag name.
// `(in '<file>')` must denote a file, and `card` is not one. It is also ambiguous with a top-level
// template of the same name, which reaches the identical format string. The position is the
// partial's own, so line 2 here is line 2 of `partials/card.html`, not of the `album.html` that
// included it.
static void test_rejects_syntax_error_in_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>card}}\n") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
               "section-opening tag has no closer", 2U, 1U, "partials/card.html");
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html",
                                     "first\n{{#media}}{{title}}\n") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A syntax error in the middle of a partial is reported like one at its end, as the first error of
// the partial. mustache4c goes on scanning the partial after it reports an error, so the diagnostic
// must be allocated past the partial's bytes rather than over them. The text after the error is
// longer than the diagnostic and holds a second error, so under ASan an overlapping message makes
// the scan read poisoned memory.
static void test_rejects_syntax_error_inside_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>card}}\n") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.gallery_config = &gallery_config,
                                          .album_current = &root};
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
               "tag opener has no closer", 2U, 1U, "partials/card.html");
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html",
                                     "first\n{{title\n"
                                     "a line of ordinary text that runs on well past the length "
                                     "of the diagnostic for the error above it\n"
                                     "{{/each}}\n"
                                     "and one more line of text after the second error\n") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "album.html", &context, &perm, NULL, &log) == -1);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// Triple braces and the ampersand tag cannot bypass the single HTML-escaping boundary, and neither
// can a partial, whose rejection is attributed to the template being rendered.
static void test_rejects_unescaped_interpolation(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/fram-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{{gallery.title}}}") != 0) {
    return;
  }

  struct GalleryConfig gallery_config;
  init_test_gallery_config(&gallery_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
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
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Log log = test_log();
    TEST_CHECK(render(root_dir, cases[i].template_name, &context, &perm, NULL, &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i].expected));
    TEST_MSG("template: %s", cases[i].template_name);
  }

cleanup:
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
    {"rejects unsafe partial name", test_rejects_unsafe_partial_name},
    {"rejects unsafe partial name in partial", test_rejects_unsafe_partial_name_in_partial},
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
    {"rejects syntax error inside partial", test_rejects_syntax_error_inside_partial},
    {"rejects unescaped interpolation", test_rejects_unescaped_interpolation},
    {NULL, NULL},
};
