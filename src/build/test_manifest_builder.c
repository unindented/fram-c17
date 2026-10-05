#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "build/manifest_builder.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/manifest.h"
#include "domain/media_item.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "test_support.h"

/**
 * @brief Creates a fixture holding `fram.toml` and `templates/album.html`, and points a
 *        configuration at it.
 *
 * `input_dir` is `photos`, `output_dir` is `public`, `templates_dir` is `templates`, and
 * `assets_dir` is `assets`, each below `root_dir`. No aggregate template is configured. Only the
 * two files exist, so no source, asset, or output is present.
 *
 * @param root_dir       Writable `mkdtemp` template. Receives the created directory path.
 * @param arena          Arena that owns the joined paths. Must not be `NULL`.
 * @param gallery_config Configuration to initialize. The caller releases it with
 *                       `gallery_config_free`. Must not be `NULL`.
 * @return The path of the fixture's `fram.toml`, owned by `arena`, or `NULL` after recording a
 *         test-plumbing failure, with the fixture removed and `gallery_config` released.
 */
static char* init_manifest_fixture(char* root_dir,
                                   struct Arena* arena,
                                   struct GalleryConfig* gallery_config) {
  gallery_config_init(gallery_config);
  if (init_fixture_dir(root_dir) == NULL) {
    return NULL;
  }
  char* config_path = path_join(root_dir, "fram.toml", arena);
  gallery_config->input_dir = path_join(root_dir, "photos", arena);
  gallery_config->output_dir = path_join(root_dir, "public", arena);
  gallery_config->templates_dir = path_join(root_dir, "templates", arena);
  gallery_config->assets_dir = path_join(root_dir, "assets", arena);
  if (!TEST_CHECK(write_fixture_file(root_dir, "fram.toml", "title = 'x'\n") == 0)) {
    goto fail;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/album.html", "album") == 0)) {
    goto fail;
  }
  if (!TEST_CHECK(config_path != NULL && gallery_config->input_dir != NULL &&
                  gallery_config->output_dir != NULL && gallery_config->templates_dir != NULL &&
                  gallery_config->assets_dir != NULL)) {
    goto fail;
  }
  return config_path;

fail:
  gallery_config_free(gallery_config);
  remove_fixture_tree(root_dir);
  return NULL;
}

/**
 * @brief Populates a manifest with one root album page and checks the input-root rejection.
 *
 * @param config      Configuration whose roots the page is checked against. Must not be `NULL`.
 * @param config_path Path of the fixture's `fram.toml`. Must not be `NULL`.
 * @param output_path Output path of the root album page, joined onto `config->output_dir`. Must not
 *                    be `NULL`.
 * @param root_key    Config key of the root the diagnostic must name. Must not be `NULL`.
 */
static void check_rejects_output_in_root(const struct GalleryConfig* config,
                                         const char* config_path,
                                         const char* output_path,
                                         const char* root_key) {
  struct Album album = {.source_dir = "", .output_path = output_path};
  const struct Album* albums[] = {&album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, config, config_path, &empty, &empty, albums, 1,
                                       err, sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "output path would write inside '%s' for '%s': '%s'",
               root_key, config->input_dir, output_path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
}

/**
 * @brief Checks that `manifest_builder_check_output_dir` rejects `config->output_dir` and creates
 *        nothing.
 *
 * @param config   Configuration whose `output_dir` lies at or below an input root. Must not be
 *                 `NULL`.
 * @param root_key Config key of the root the diagnostic must name. Must not be `NULL`.
 */
static void check_rejects_output_dir_in_root(const struct GalleryConfig* config,
                                             const char* root_key) {
  const bool had_output_dir = access(config->output_dir, F_OK) == 0;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_check_output_dir(config, err, sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "output directory would write inside '%s': '%s'",
               root_key, config->output_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    return;
  }
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);
  TEST_CHECK((access(config->output_dir, F_OK) == 0) == had_output_dir);
}

// Every album, derivative, original, aggregate, and asset output is registered exactly once.
static void test_registers_complete_output_set(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* source_path = path_join(config.input_dir, "x.jpg", &arena);
  char* asset_path = path_join(config.assets_dir, "a.txt", &arena);
  char* album_output = path_join(config.output_dir, "index.html", &arena);
  char* small = path_join(config.output_dir, "_fram/r/s/x.jpg", &arena);
  char* medium = path_join(config.output_dir, "_fram/r/m/x.jpg", &arena);
  char* large = path_join(config.output_dir, "_fram/r/l/x.jpg", &arena);
  char* original = path_join(config.output_dir, "_fram/originals/x.jpg", &arena);
  static const char* const aggregates[] = {"map.xml"};
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  struct MediaDerivative derivatives[] = {
      {.name = "s", .path = small},
      {.name = "m", .path = medium},
      {.name = "l", .path = large},
  };
  struct MediaItem media = {.source_path = source_path,
                            .derivatives = derivatives,
                            .derivative_count = 3,
                            .original_path = original};
  struct MediaItem* media_items[] = {&media};
  struct Album album = {
      .source_dir = "", .output_path = album_output, .media = media_items, .media_count = 1};
  const struct Album* albums[] = {&album};
  struct PathList sources;
  struct PathList assets;
  path_list_init(&sources);
  path_list_init(&assets);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  if (!TEST_CHECK(write_fixture_file(root_dir, "photos/x.jpg", "source") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/map.xml", "map") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "assets/a.txt", "asset") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(source_path != NULL && asset_path != NULL && album_output != NULL &&
                  small != NULL && medium != NULL && large != NULL && original != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(path_list_push(&sources, source_path) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(path_list_push(&assets, asset_path) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &sources, &assets, albums,
                                       1, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_MSG("actual: '%s'", err);
  TEST_CHECK(manifest.count == 7);

cleanup:
  manifest_free(&manifest);
  path_list_free(&sources);
  path_list_free(&assets);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A `templates_dir` that does not exist holds no file an output could overwrite, so it claims
// nothing and the manifest still populates. The build refuses a missing `templates_dir` before the
// manifest runs, as `test_reports_absent_templates_dir` in `src/app/test_cmd_build.c` pins, so this
// is the module's own contract rather than a path the build reaches.
static void test_accepts_missing_templates_dir(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* album_output = path_join(config.output_dir, "index.html", &arena);
  char* templates_dir = path_join(root_dir, "missing", &arena);
  struct Album album = {.source_dir = "", .output_path = album_output};
  const struct Album* albums[] = {&album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  if (!TEST_CHECK(album_output != NULL && templates_dir != NULL)) {
    goto cleanup;
  }
  config.templates_dir = templates_dir;

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 1,
                                       err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(manifest.count == 1);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An output beside an input root, in a directory whose name only starts with the root's name, is
// accepted, because the root check compares whole directories rather than path prefixes.
static void test_accepts_output_beside_input_roots(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  config.output_dir = root_dir;
  char* photos_output = path_join(root_dir, "photosx/index.html", &arena);
  char* templates_output = path_join(root_dir, "templatesx/index.html", &arena);
  char* assets_output = path_join(root_dir, "assetsx/index.html", &arena);
  struct Album photos_album = {.source_dir = "a", .output_path = photos_output};
  struct Album templates_album = {.source_dir = "b", .output_path = templates_output};
  struct Album assets_album = {.source_dir = "c", .output_path = assets_output};
  const struct Album* albums[] = {&photos_album, &templates_album, &assets_album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  if (!TEST_CHECK(write_fixture_file(root_dir, "photos/x.jpg", "source") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "assets/a.css", "asset") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "photosx/old.html", "old") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(photos_output != NULL && templates_output != NULL && assets_output != NULL)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 3,
                                       err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_MSG("actual: '%s'", err);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An `output_dir` outside every input root passes the early check whether or not it exists yet.
// That includes a directory whose name only starts with a root's name, and one that holds the roots
// rather than lying inside them, which is the `output_dir = "."` layout.
static void test_check_output_dir_accepts_dir_outside_input_roots(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  if (init_manifest_fixture(root_dir, &arena, &config) == NULL) {
    arena_free(&arena);
    return;
  }
  const char* const output_dirs[] = {
      root_dir,
      path_join(root_dir, "public", &arena),
      path_join(root_dir, "photosx/new", &arena),
      path_join(root_dir, "photos/../public/new", &arena),
  };
  if (!TEST_CHECK(write_fixture_file(root_dir, "photos/x.jpg", "source") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "assets/a.css", "asset") == 0)) {
    goto cleanup;
  }
  for (size_t i = 0; i < sizeof(output_dirs) / sizeof(output_dirs[0]); i++) {
    if (!TEST_CHECK(output_dirs[i] != NULL)) {
      goto cleanup;
    }
    config.output_dir = output_dirs[i];
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(manifest_builder_check_output_dir(&config, err, sizeof(err)) == 0);
    TEST_MSG("output_dir: '%s', actual: '%s'", output_dirs[i], err);
  }

cleanup:
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A configured template's output path is its name joined below `output_dir`, nested directories
// included. `site_writer` writes to this same path, so it is the one the manifest checked.
static void test_derive_template_output_joins_output_dir(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* top = manifest_builder_derive_template_output("public", "index.html", &arena);
  const char* nested = manifest_builder_derive_template_output("public", "feeds/map.xml", &arena);
  TEST_ASSERT(top != NULL && nested != NULL);
  TEST_CHECK(strcmp(top, "public/index.html") == 0);
  TEST_CHECK(strcmp(nested, "public/feeds/map.xml") == 0);
  arena_free(&arena);
}

// An asset's output path keeps its path below `assets_dir`, rooted at the output `assets/`
// directory. `site_writer` copies to this same path, so it is the one the manifest checked.
static void test_derive_asset_output_joins_output_assets_dir(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* output = manifest_builder_derive_asset_output("public", "icons/a.svg", &arena);
  TEST_ASSERT(output != NULL);
  TEST_CHECK(strcmp(output, "public/assets/icons/a.svg") == 0);
  arena_free(&arena);
}

// Two albums claiming the same output path are rejected with a diagnostic naming the path.
static void test_rejects_duplicate(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* output_path = path_join(config.output_dir, "trip/index.html", &arena);
  char* first_label = path_join(config.input_dir, "a", &arena);
  char* second_label = path_join(config.input_dir, "b", &arena);
  struct Album first_album = {.source_dir = "a", .output_path = output_path};
  struct Album second_album = {.source_dir = "b", .output_path = output_path};
  const struct Album* albums[] = {&first_album, &second_album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(output_path != NULL && first_label != NULL && second_label != NULL)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 2,
                                       err, sizeof(err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               first_label, second_label, output_path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Two albums whose page paths differ only in ASCII case claim one file on a case-insensitive
// filesystem such as the macOS default, where the second write would silently replace the first.
// They are rejected as a duplicate on every platform, so a gallery that builds on Linux does not
// break on macOS. The trailing path is the second claim's own spelling.
static void test_rejects_case_folded_duplicate(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* first_output = path_join(config.output_dir, "Trip/index.html", &arena);
  char* second_output = path_join(config.output_dir, "trip/index.html", &arena);
  char* first_label = path_join(config.input_dir, "a", &arena);
  char* second_label = path_join(config.input_dir, "b", &arena);
  struct Album first_album = {.source_dir = "a", .output_path = first_output};
  struct Album second_album = {.source_dir = "b", .output_path = second_output};
  const struct Album* albums[] = {&first_album, &second_album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(first_output != NULL && second_output != NULL && first_label != NULL &&
                  second_label != NULL)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 2,
                                       err, sizeof(err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               first_label, second_label, second_output);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Two albums whose page paths collide as a file and a directory, where one is a `/`-delimited
// prefix of the other, are rejected even though neither is a duplicate. The two producers lead the
// message ahead of the one unbounded path. The ancestor path is not repeated because it is a prefix
// of the path that is printed.
static void test_rejects_prefix_collision(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* file_output = path_join(config.output_dir, "trip", &arena);
  char* nested_output = path_join(config.output_dir, "trip/index.html", &arena);
  char* file_label = path_join(config.input_dir, "a", &arena);
  char* nested_label = path_join(config.input_dir, "b", &arena);
  struct Album file_album = {.source_dir = "a", .output_path = file_output};
  struct Album nested_album = {.source_dir = "b", .output_path = nested_output};
  const struct Album* albums[] = {&file_album, &nested_album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(file_output != NULL && nested_output != NULL && file_label != NULL &&
                  nested_label != NULL)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 2,
                                       err, sizeof(err)) == -1);
  expected_len = snprintf(expected, sizeof(expected),
                          "output path for '%s' nests under output path for '%s': '%s'",
                          nested_label, file_label, nested_output);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// The root album and a configured `index.html` aggregate collide before any writer runs. The root
// is labeled by `input_dir`, a real path, rather than a placeholder a sub-album could share.
static void test_rejects_album_aggregate_collision(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* output_path = path_join(config.output_dir, "index.html", &arena);
  static const char* const aggregates[] = {"index.html"};
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  struct Album album = {.source_dir = "", .output_path = output_path};
  const struct Album* albums[] = {&album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/index.html", "aggregate") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(output_path != NULL)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 1,
                                       err, sizeof(err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               config.input_dir, "aggregate_templates[0]", output_path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// One name listed twice in `aggregate_templates` is rejected with each entry named by its index
// rather than by its own name. A bare template name reports it as `for 'dup.html' and 'dup.html'`,
// which reads as a value colliding with itself and says nothing about which entry to remove.
static void test_rejects_duplicate_template(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* album_output = path_join(config.output_dir, "index.html", &arena);
  char* template_output = path_join(config.output_dir, "dup.html", &arena);
  static const char* const aggregates[] = {"dup.html", "dup.html"};
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 2;
  struct Album album = {.source_dir = "", .output_path = album_output};
  const struct Album* albums[] = {&album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(album_output != NULL && template_output != NULL)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 1,
                                       err, sizeof(err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               "aggregate_templates[0]", "aggregate_templates[1]", template_output);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `manifest_builder_populate` rejects an output path that names one of the build's own input files,
// so a build cannot overwrite its own source. Here an original output is a hard link to its own
// media source, so the two paths differ and only filesystem identity sees the collision. An output
// inside `input_dir` fails the root check first, as `test_rejects_output_in_input_dir` pins.
static void test_rejects_input_overwrite(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* source_path = path_join(config.input_dir, "x.jpg", &arena);
  char* album_output = path_join(config.output_dir, "index.html", &arena);
  char* small = path_join(config.output_dir, "small.jpg", &arena);
  char* large = path_join(config.output_dir, "large.jpg", &arena);
  char* original = path_join(config.output_dir, "x.jpg", &arena);
  struct MediaDerivative derivatives[] = {
      {.name = "s", .path = small},
      {.name = "l", .path = large},
  };
  struct MediaItem media = {.source_path = source_path,
                            .derivatives = derivatives,
                            .derivative_count = 2,
                            .original_path = original};
  struct MediaItem* media_items[] = {&media};
  struct Album album = {
      .source_dir = "", .output_path = album_output, .media = media_items, .media_count = 1};
  const struct Album* albums[] = {&album};
  struct PathList sources;
  struct PathList assets;
  path_list_init(&sources);
  path_list_init(&assets);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(write_fixture_file(root_dir, "photos/x.jpg", "source") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(source_path != NULL && album_output != NULL && small != NULL && large != NULL &&
                  original != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_mkdir_p(config.output_dir, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(link(source_path, original) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(path_list_push(&sources, source_path) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &sources, &assets, albums,
                                       1, err, sizeof(err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               source_path, original);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&sources);
  path_list_free(&assets);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// The input check finds an aimed-at source wherever it falls among many claimed inputs, and still
// accepts an existing output that is not an input. The sources are created in reverse name order,
// so the order they are claimed in differs from the order their identities sort in. Each aimed-at
// original is a hard link to its source, for the reason `test_rejects_input_overwrite` gives.
static void test_rejects_input_overwrite_among_many_inputs(void) {
  enum { SOURCE_COUNT = 32 };
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* existing_output = path_join(config.output_dir, "existing.jpg", &arena);
  char* album_output = path_join(config.output_dir, "index.html", &arena);
  struct PathList sources;
  struct PathList assets;
  path_list_init(&sources);
  path_list_init(&assets);
  if (!TEST_CHECK(write_fixture_file(root_dir, "public/existing.jpg", "output") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(existing_output != NULL && album_output != NULL)) {
    goto cleanup;
  }
  for (size_t i = SOURCE_COUNT; i > 0; i--) {
    char relative[32];
    (void)snprintf(relative, sizeof(relative), "photos/%02zu.jpg", i - 1);
    if (!TEST_CHECK(write_fixture_file(root_dir, relative, "source") == 0)) {
      goto cleanup;
    }
  }
  for (size_t i = 0; i < SOURCE_COUNT; i++) {
    char name[16];
    (void)snprintf(name, sizeof(name), "%02zu.jpg", i);
    char* source_path = path_join(config.input_dir, name, &arena);
    if (!TEST_CHECK(source_path != NULL && path_list_push(&sources, source_path) == 0)) {
      goto cleanup;
    }
  }

  static const size_t targets[] = {0, SOURCE_COUNT / 2, SOURCE_COUNT - 1};
  for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
    const char* target = sources.items[targets[i]];
    char name[16];
    (void)snprintf(name, sizeof(name), "%02zu.jpg", targets[i]);
    char* original = path_join(config.output_dir, name, &arena);
    if (!TEST_CHECK(original != NULL && link(target, original) == 0)) {
      goto cleanup;
    }
    char expected[ERROR_MESSAGE_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "output path would overwrite build input for '%s': '%s'", target, original);
    if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
      goto cleanup;
    }
    // The existing output comes first, so a lookup that wrongly matched it would name it instead.
    struct MediaDerivative derivatives[] = {{.name = "s", .path = existing_output}};
    struct MediaItem media = {.source_path = target,
                              .derivatives = derivatives,
                              .derivative_count = 1,
                              .original_path = original};
    struct MediaItem* media_items[] = {&media};
    struct Album album = {
        .source_dir = "", .output_path = album_output, .media = media_items, .media_count = 1};
    const struct Album* albums[] = {&album};
    struct Manifest manifest;
    manifest_init(&manifest);
    char err[ERROR_MESSAGE_SIZE] = "";

    TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &sources, &assets, albums,
                                         1, err, sizeof(err)) == -1);
    TEST_CHECK(strcmp(err, expected) == 0);
    TEST_MSG("target %zu: '%s'", targets[i], err);
    manifest_free(&manifest);
  }

cleanup:
  path_list_free(&sources);
  path_list_free(&assets);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// The config file is an input like any other, so an output path naming it is rejected. Without this
// the build would overwrite the file that configured it and still report success: `output_dir` at
// the gallery root plus an aggregate template named `fram.toml` is all it takes, and the loss is
// unrecoverable.
static void test_rejects_config_overwrite(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* album_output = path_join(config.output_dir, "index.html", &arena);
  static const char* const aggregates[] = {"fram.toml"};
  config.output_dir = root_dir;
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  struct Album album = {.source_dir = "", .output_path = album_output};
  const struct Album* albums[] = {&album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(album_output != NULL)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 1,
                                       err, sizeof(err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "aggregate_templates[0]", config_path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Every file below `templates_dir` is a build input, so an output naming the album template or a
// partial is rejected. An output inside `templates_dir` fails the root check first, as
// `test_rejects_output_in_templates_dir` pins, so each output here sits in `output_dir` as a hard
// link to a template, which only the identity claim can see. The partial is named in no
// configuration, so only the walk of the template tree can claim it, which is the case a claim of
// just the configured templates would miss.
static void test_rejects_template_overwrite(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* template_path = path_join(config.templates_dir, "album.html", &arena);
  char* partial_path = path_join(config.templates_dir, "partials/card.html", &arena);
  char* template_output = path_join(config.output_dir, "index.html", &arena);
  char* partial_output = path_join(config.output_dir, "card.html", &arena);
  char* album_output = path_join(config.output_dir, "page.html", &arena);
  struct PathList empty;
  path_list_init(&empty);
  struct Album template_album = {.source_dir = "", .output_path = template_output};
  const struct Album* template_albums[] = {&template_album};
  struct Manifest template_manifest;
  manifest_init(&template_manifest);
  char template_err[ERROR_MESSAGE_SIZE] = "";
  static const char* const aggregates[] = {"card.html"};
  struct Album partial_album = {.source_dir = "", .output_path = album_output};
  const struct Album* partial_albums[] = {&partial_album};
  struct Manifest partial_manifest;
  manifest_init(&partial_manifest);
  char partial_err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/partials/card.html", "card") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(template_path != NULL && partial_path != NULL && template_output != NULL &&
                  partial_output != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_mkdir_p(config.output_dir, NULL, 0) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(link(template_path, template_output) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(link(partial_path, partial_output) == 0)) {
    goto cleanup;
  }

  // First case: the album page is a hard link to the configured album template.
  TEST_CHECK(manifest_builder_populate(&template_manifest, &config, config_path, &empty, &empty,
                                       template_albums, 1, template_err,
                                       sizeof(template_err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               config.input_dir, template_output);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(template_err, expected) == 0);

  // Second case: an aggregate lands on a hard link to an unconfigured partial.
  if (!TEST_CHECK(album_output != NULL)) {
    goto cleanup;
  }
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  TEST_CHECK(manifest_builder_populate(&partial_manifest, &config, config_path, &empty, &empty,
                                       partial_albums, 1, partial_err, sizeof(partial_err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "aggregate_templates[0]", partial_output);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(partial_err, expected) == 0);

cleanup:
  manifest_free(&partial_manifest);
  manifest_free(&template_manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An `output_dir` at or below an input root is rejected before anything walks or writes, whether or
// not it exists yet, and the check creates none of it. A missing `output_dir` is judged by the
// directory it would be created in, so a nested path, a `..` past a missing component, and a path
// through a symlinked root all land where `fs_mkdir_p` would put them.
static void test_check_output_dir_rejects_dir_in_input_root(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  if (init_manifest_fixture(root_dir, &arena, &config) == NULL) {
    arena_free(&arena);
    return;
  }
  const char* input_link = path_join(root_dir, "photos-link", &arena);
  const char* const input_output_dirs[] = {
      config.input_dir,
      path_join(root_dir, "photos/public", &arena),
      path_join(root_dir, "photos/new/deeper", &arena),
      path_join(root_dir, "missing/../photos/new", &arena),
      path_join(root_dir, "photos-link/new", &arena),
  };
  if (!TEST_CHECK(write_fixture_file(root_dir, "photos/public/old.html", "old") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "assets/a.css", "asset") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(input_link != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(symlink(config.input_dir, input_link) == 0);
  for (size_t i = 0; i < sizeof(input_output_dirs) / sizeof(input_output_dirs[0]); i++) {
    if (!TEST_CHECK(input_output_dirs[i] != NULL)) {
      goto cleanup;
    }
    config.output_dir = input_output_dirs[i];
    check_rejects_output_dir_in_root(&config, "input_dir");
  }

  config.output_dir = path_join(root_dir, "templates/partials", &arena);
  if (!TEST_CHECK(config.output_dir != NULL)) {
    goto cleanup;
  }
  check_rejects_output_dir_in_root(&config, "templates_dir");
  config.output_dir = path_join(root_dir, "assets/generated", &arena);
  if (!TEST_CHECK(config.output_dir != NULL)) {
    goto cleanup;
  }
  check_rejects_output_dir_in_root(&config, "assets_dir");

cleanup:
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An output that would land below `input_dir` is rejected even though no file sits at its path yet,
// because the next build could discover it as media. The root is compared by identity, so an
// alternate spelling of it, a symlink to it, a symlink inside `output_dir` that leads into it, and
// an `output_dir` inside it are rejected the same way.
static void test_rejects_output_in_input_dir(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  const char* input_dir = config.input_dir;
  const char* input_link = path_join(root_dir, "photos-link", &arena);
  const char* output_dir = path_join(root_dir, "out", &arena);
  const char* output_link = path_join(root_dir, "out/link", &arena);
  struct Album album = {.source_dir = ""};
  const struct Album* albums[] = {&album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(write_fixture_file(root_dir, "photos/x.jpg", "source") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(input_link != NULL && output_dir != NULL && output_link != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(symlink(input_dir, input_link) == 0);
  TEST_CHECK(fs_mkdir_p(output_dir, NULL, 0) == 0);
  TEST_CHECK(symlink(input_dir, output_link) == 0);

  // The output is a new file below the root, reached from an `output_dir` that holds the root.
  config.output_dir = root_dir;
  check_rejects_output_in_root(&config, config_path, path_join(root_dir, "photos/b.html", &arena),
                               "input_dir");

  // The root is configured through a symlink and spelled with a `./` component.
  config.input_dir = path_join(root_dir, "./photos-link", &arena);
  check_rejects_output_in_root(&config, config_path,
                               path_join(root_dir, "photos/sub/b.html", &arena), "input_dir");

  // A symlink inside `output_dir` leads into the root.
  config.input_dir = input_dir;
  config.output_dir = output_dir;
  check_rejects_output_in_root(&config, config_path, path_join(output_dir, "link/b.html", &arena),
                               "input_dir");

  // `output_dir` itself lies inside the root, and does not exist yet. Populate runs the early check
  // itself, so a caller that skipped it is still refused, with the early check's diagnostic.
  config.output_dir = path_join(root_dir, "photos/public", &arena);
  if (!TEST_CHECK(config.output_dir != NULL)) {
    goto cleanup;
  }
  album.output_path = path_join(config.output_dir, "index.html", &arena);
  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 1,
                                       err, sizeof(err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "output directory would write inside 'input_dir': '%s'",
               config.output_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);
  TEST_CHECK(manifest.count == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An output that would land below `templates_dir` is rejected even though no file sits at its path
// yet, because the same build's render could read it back as a partial. The check covers configured
// aggregate outputs as well as album pages.
static void test_rejects_output_in_templates_dir(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  config.output_dir = root_dir;
  check_rejects_output_in_root(&config, config_path,
                               path_join(root_dir, "templates/partials/b.html", &arena),
                               "templates_dir");

  // A configured aggregate whose name leads into the template tree is rejected by its list entry.
  char* album_output = path_join(root_dir, "public/index.html", &arena);
  static const char* const aggregates[] = {"templates/index.html"};
  struct Album album = {.source_dir = "", .output_path = album_output};
  const struct Album* albums[] = {&album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(album_output != NULL)) {
    goto cleanup;
  }
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 1,
                                       err, sizeof(err)) == -1);
  expected_len = snprintf(expected, sizeof(expected),
                          "output path would write inside 'templates_dir' for "
                          "'aggregate_templates[0]': '%s/templates/index.html'",
                          root_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An output that would land below `assets_dir` is rejected, because the next build would copy it as
// an asset.
static void test_rejects_output_in_assets_dir(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "assets/a.css", "asset") == 0)) {
    goto cleanup;
  }
  config.output_dir = root_dir;
  check_rejects_output_in_root(&config, config_path, path_join(root_dir, "assets/b.html", &arena),
                               "assets_dir");

cleanup:
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A `templates_dir` that exists but cannot be walked fails the manifest rather than claiming
// nothing, because a partial the walk could not see would stay overwritable. A regular file in
// place of the directory is the portable way to make the walk fail. The reason names the path the
// walk failed on, so the configured root is not repeated ahead of it.
static void test_rejects_unlistable_templates_dir(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* album_output = path_join(config.output_dir, "index.html", &arena);
  char* templates_dir = path_join(root_dir, "templates-file", &arena);
  struct Album album = {.source_dir = "", .output_path = album_output};
  const struct Album* albums[] = {&album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates-file", "not a directory") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(album_output != NULL && templates_dir != NULL)) {
    goto cleanup;
  }
  config.templates_dir = templates_dir;

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 1,
                                       err, sizeof(err)) == -1);
  expected_len = snprintf(expected, sizeof(expected),
                          "failed to list template files: cannot open directory: %s ('%s')",
                          error_system_message(reason, sizeof(reason), ENOTDIR), templates_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_CHECK(manifest.count == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A configured template whose output path exceeds the whole-path limit is rejected, and the
// diagnostic still carries the limit. This branch is reachable only when the template name is
// longer than `OUTPUT_PATH_RELATIVE_LEN_MAX`, which is larger than `ERROR_MESSAGE_SIZE`, so leading
// with the name would make the limit clause unreachable at every triggering input. The message
// would be 511 bytes of filename and nothing else. Asserting the head is what pins the ordering.
static void test_rejects_oversize_template_path(void) {
  char name[OUTPUT_PATH_RELATIVE_LEN_MAX + 64];
  memset(name, 'a', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  const char* aggregates[] = {name};

  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* album_output = path_join(config.output_dir, "index.html", &arena);
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  struct Album album = {.source_dir = "", .output_path = album_output};
  const struct Album* albums[] = {&album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected_head[128];
  int expected_head_len = 0;
  size_t err_actual_len = 0;
  if (!TEST_CHECK(album_output != NULL)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 1,
                                       err, sizeof(err)) == -1);
  expected_head_len = snprintf(expected_head, sizeof(expected_head),
                               "output path exceeds max output path length (%zu bytes) at %zu "
                               "bytes (for 'aggregate_templates[0]'): '",
                               (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX, sizeof(name) - 1);
  TEST_CHECK(expected_head_len > 0 && (size_t)expected_head_len < sizeof(expected_head));
  TEST_CHECK(strncmp(err, expected_head, (size_t)expected_head_len) == 0);
  // The head is a prefix, so on its own it says nothing about the rest of the buffer. The name is
  // `OUTPUT_PATH_RELATIVE_LEN_MAX + 63` bytes, so the composed message cannot fit `err` and must
  // come back truncated: asserting the exact length proves that is still true. The marker proves
  // the cut is visible to the user. The `...` is spelled out because `TRUNCATION_MARKER` is
  // file-local to `core/error.c`. Changing it there must update this line.
  err_actual_len = strlen(err);
  TEST_CHECK(err_actual_len == ERROR_MESSAGE_SIZE - 1);
  TEST_CHECK(err_actual_len >= 3 && strcmp(err + err_actual_len - 3, "...") == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A configured template with one overlong path segment is rejected against the per-filename limit
// rather than the whole-path one, so the two limits stay distinguishable. The whole message fits,
// so it is asserted in full.
static void test_rejects_oversize_template_segment(void) {
  char name[FILENAME_LEN_MAX + 32];
  memset(name, 'b', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  const char* aggregates[] = {name};

  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* album_output = path_join(config.output_dir, "index.html", &arena);
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  struct Album album = {.source_dir = "", .output_path = album_output};
  const struct Album* albums[] = {&album};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(album_output != NULL)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, &empty, albums, 1,
                                       err, sizeof(err)) == -1);
  expected_len = snprintf(expected, sizeof(expected),
                          "output path segment exceeds max filename length (%zu bytes) at %zu "
                          "bytes (for 'aggregate_templates[0]'): '%s'",
                          (size_t)FILENAME_LEN_MAX, sizeof(name) - 1, name);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&empty);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An enumerated asset that does not lie below `assets_dir` has no output below `assets/`, so it is
// rejected rather than registered at a path the copy would not reproduce. The sibling directory
// shares `assets` as a text prefix, so only the separator check tells the two apart.
static void test_rejects_asset_not_below_root(void) {
  char root_dir[] = "/tmp/fram-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct GalleryConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  if (config_path == NULL) {
    arena_free(&arena);
    return;
  }
  char* album_output = path_join(config.output_dir, "index.html", &arena);
  char* asset_path = path_join(root_dir, "assets-old/a.txt", &arena);
  struct Album album = {.source_dir = "", .output_path = album_output};
  const struct Album* albums[] = {&album};
  struct PathList sources;
  struct PathList assets;
  path_list_init(&sources);
  path_list_init(&assets);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(album_output != NULL && asset_path != NULL)) {
    goto cleanup;
  }
  if (!TEST_CHECK(path_list_push(&assets, asset_path) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &sources, &assets, albums,
                                       1, err, sizeof(err)) == -1);
  expected_len = snprintf(expected, sizeof(expected),
                          "asset path is not below configured assets directory: '%s'", asset_path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  manifest_free(&manifest);
  path_list_free(&sources);
  path_list_free(&assets);
  gallery_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// The writing this module guards lives in `site_writer` and `derivative_renderer`, and is tested in
// `src/build/test_site_writer.c` and `src/build/test_derivative_renderer.c`. Album and media output
// paths are planned in `album_scanner` and are tested in `src/build/test_album_scanner.c`.

TEST_LIST = {
    {"registers complete output set", test_registers_complete_output_set},
    {"accepts missing templates dir", test_accepts_missing_templates_dir},
    {"accepts output beside input roots", test_accepts_output_beside_input_roots},
    {"check output dir accepts dir outside input roots",
     test_check_output_dir_accepts_dir_outside_input_roots},
    {"derive template output joins output dir", test_derive_template_output_joins_output_dir},
    {"derive asset output joins output assets dir",
     test_derive_asset_output_joins_output_assets_dir},
    {"rejects duplicate", test_rejects_duplicate},
    {"rejects case folded duplicate", test_rejects_case_folded_duplicate},
    {"rejects prefix collision", test_rejects_prefix_collision},
    {"rejects album aggregate collision", test_rejects_album_aggregate_collision},
    {"rejects duplicate template", test_rejects_duplicate_template},
    {"rejects input overwrite", test_rejects_input_overwrite},
    {"rejects input overwrite among many inputs", test_rejects_input_overwrite_among_many_inputs},
    {"rejects config overwrite", test_rejects_config_overwrite},
    {"rejects template overwrite", test_rejects_template_overwrite},
    {"check output dir rejects dir in input root", test_check_output_dir_rejects_dir_in_input_root},
    {"rejects output in input dir", test_rejects_output_in_input_dir},
    {"rejects output in templates dir", test_rejects_output_in_templates_dir},
    {"rejects output in assets dir", test_rejects_output_in_assets_dir},
    {"rejects unlistable templates dir", test_rejects_unlistable_templates_dir},
    {"rejects oversize template path", test_rejects_oversize_template_path},
    {"rejects oversize template segment", test_rejects_oversize_template_segment},
    {"rejects asset not below root", test_rejects_asset_not_below_root},
    {NULL, NULL},
};
