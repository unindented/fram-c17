#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "build/manifest_builder.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/manifest.h"
#include "domain/media_item.h"
#include "os_memory.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// Every test here runs against the in-memory host. The identity checks see what they would see on
// disk: two spellings of one directory, a symlink and its target, and two hard links to one file
// share an identity, and `..` resolves against the directory actually reached.

/** Path of the fixture's configuration file. */
#define CONFIG_PATH S("gallery/fram.toml")

/**
 * @brief Creates a host holding `gallery/fram.toml` and `gallery/templates/album.html`, and points
 *        a configuration at it.
 *
 * `input_dir` is `gallery/photos`, `output_dir` is `gallery/public`, `templates_dir` is
 * `gallery/templates`, and `static_dir` is `gallery/static`. No aggregate template is configured.
 * Only the two files exist, so no source, static file, or output is present.
 *
 * @param gallery_config Configuration to initialize. Must not be `NULL`.
 * @return The host.
 */
static struct Os* init_manifest_fixture(struct GalleryConfig* gallery_config) {
  gallery_config_init(gallery_config);
  gallery_config->input_dir = S("gallery/photos");
  gallery_config->output_dir = S("gallery/public");
  gallery_config->templates_dir = S("gallery/templates");
  gallery_config->static_dir = S("gallery/static");
  struct Os* os = os_memory();
  os_memory_write(os, "gallery/fram.toml", "title = 'x'\n");
  os_memory_write(os, "gallery/templates/album.html", "album");
  return os;
}

/**
 * @brief Populates a manifest from albums, as the build does.
 *
 * The manifest's data goes in a reservation of its own, never in the log's arena, so a failure the
 * log records cannot grow over the paths the populate is still using.
 *
 * @param manifest       Manifest that receives the outputs. Must not be `NULL`.
 * @param gallery_config Configuration to populate from. Must not be `NULL`.
 * @param source_paths   Claimed media sources, or `NULL` for none.
 * @param static_paths   Enumerated static files, or `NULL` for none.
 * @param albums         Array of `album_count` albums. Must not be `NULL`.
 * @param album_count    Number of albums.
 * @param os             Host the fixture is on. Must not be `NULL`.
 * @param log            Log that receives every error. Must not be `NULL`.
 * @return The result of `manifest_builder_populate`.
 */
static int populate(struct Manifest* manifest,
                    const struct GalleryConfig* gallery_config,
                    const struct PathList* source_paths,
                    const struct PathList* static_paths,
                    const struct Album* const* albums,
                    ptrdiff_t album_count,
                    struct Os* os,
                    struct Log* log) {
  static const struct PathList empty = {0};
  struct Arena perm = test_arena();
  return manifest_builder_populate(manifest, gallery_config, CONFIG_PATH,
                                   source_paths != NULL ? source_paths : &empty,
                                   static_paths != NULL ? static_paths : &empty, albums,
                                   album_count, os, &perm, test_arena(), log);
}

/**
 * @brief Populates a manifest with one root album page and checks the input-root rejection.
 *
 * @param gallery_config Configuration whose roots the page is checked against. Must not be `NULL`.
 * @param output_path    Terminated output path of the root album page. Must not be `NULL`.
 * @param root_key       Config key of the root the diagnostic must name. Must not be `NULL`.
 * @param os             Host the fixture is on. Must not be `NULL`.
 */
static void check_rejects_output_in_root(const struct GalleryConfig* gallery_config,
                                         const char* output_path,
                                         const char* root_key,
                                         struct Os* os) {
  struct Album album = {.source_dir = S(""), .output_path = str_from_cstr(output_path)};
  const struct Album* albums[] = {&album};
  struct Manifest manifest = {0};
  struct Log log = test_log();
  TEST_CHECK(populate(&manifest, gallery_config, NULL, NULL, albums, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "output path would write inside '%s' for '%.*s': '%s'\n", root_key,
                         STR_FMT(gallery_config->input_dir), output_path));
}

/**
 * @brief Checks that `manifest_builder_check_output_dir` rejects `gallery_config->output_dir` and
 *        creates nothing.
 *
 * @param gallery_config Configuration whose `output_dir` lies at or below an input root. Must not
 *                       be `NULL`.
 * @param root_key       Config key of the root the diagnostic must name. Must not be `NULL`.
 * @param os             Host the fixture is on. Must not be `NULL`.
 */
static void check_rejects_output_dir_in_root(const struct GalleryConfig* gallery_config,
                                             const char* root_key,
                                             struct Os* os) {
  struct Arena arena = test_arena();
  const char* output_dir = str_cstr(gallery_config->output_dir, &arena);
  const bool had_output_dir = os_memory_has_path(os, output_dir);
  struct Log log = test_log();
  TEST_CHECK(manifest_builder_check_output_dir(gallery_config, os, test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log, "output directory would write inside '%s': '%.*s'\n", root_key,
                         STR_FMT(gallery_config->output_dir)));
  TEST_CHECK(os_memory_has_path(os, output_dir) == had_output_dir);
  TEST_MSG("output_dir: '%s'", output_dir);
}

// Every album, derivative, original, aggregate, and static file output is registered exactly once.
static void test_registers_complete_output_set(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/photos/x.jpg", "source");
  os_memory_write(os, "gallery/templates/map.xml", "map");
  os_memory_write(os, "gallery/static/a.txt", "static");
  static const struct Str aggregates[] = {STR_INIT("map.xml")};
  gallery_config.aggregate_templates = aggregates;
  gallery_config.aggregate_template_count = 1;
  struct MediaDerivative derivatives[] = {
      {.name = S("s"), .path = S("gallery/public/_fram/r/s/x.jpg")},
      {.name = S("m"), .path = S("gallery/public/_fram/r/m/x.jpg")},
      {.name = S("l"), .path = S("gallery/public/_fram/r/l/x.jpg")},
  };
  struct MediaItem media = {.source_path = S("gallery/photos/x.jpg"),
                            .derivatives = derivatives,
                            .derivative_count = 3,
                            .original_path = S("gallery/public/_fram/originals/x.jpg")};
  struct MediaItem* media_items[] = {&media};
  struct Album album = {.source_dir = S(""),
                        .output_path = S("gallery/public/index.html"),
                        .media = media_items,
                        .media_count = 1};
  const struct Album* albums[] = {&album};
  struct Arena arena = test_arena();
  struct PathList sources = {0};
  struct PathList static_paths = {0};
  path_list_push(&sources, S("gallery/photos/x.jpg"), &arena);
  path_list_push(&static_paths, S("gallery/static/a.txt"), &arena);
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, &sources, &static_paths, albums, 1, os, &log) ==
             0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(manifest.count == 7);
}

// A `templates_dir` that does not exist holds no file an output could overwrite, so it claims
// nothing and the manifest still populates. The build refuses a missing `templates_dir` before the
// manifest runs, as `test_reports_absent_templates_dir` in `src/app/test_cmd_build.c` pins, so this
// is the module's own contract rather than a path the build reaches.
static void test_accepts_missing_templates_dir(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  gallery_config.templates_dir = S("gallery/missing");
  struct Album album = {.source_dir = S(""), .output_path = S("gallery/public/index.html")};
  const struct Album* albums[] = {&album};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 1, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(manifest.count == 1);
}

// An output beside an input root, in a directory whose name only starts with the root's name, is
// accepted, because the root check compares whole directories rather than path prefixes.
static void test_accepts_output_beside_input_roots(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/photos/x.jpg", "source");
  os_memory_write(os, "gallery/static/a.css", "static");
  os_memory_write(os, "gallery/photosx/old.html", "old");
  gallery_config.output_dir = S("gallery");
  struct Album photos_album = {.source_dir = S("a"),
                               .output_path = S("gallery/photosx/index.html")};
  struct Album templates_album = {.source_dir = S("b"),
                                  .output_path = S("gallery/templatesx/index.html")};
  struct Album static_album = {.source_dir = S("c"),
                               .output_path = S("gallery/staticx/index.html")};
  const struct Album* albums[] = {&photos_album, &templates_album, &static_album};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 3, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// An `output_dir` outside every input root passes the early check whether or not it exists yet.
// That includes a directory whose name only starts with a root's name, and one that holds the roots
// rather than lying inside them, which is the `output_dir = "."` layout.
static void test_check_output_dir_accepts_dir_outside_input_roots(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/photos/x.jpg", "source");
  os_memory_write(os, "gallery/static/a.css", "static");
  static const struct Str output_dirs[] = {
      STR_INIT("gallery"),
      STR_INIT("gallery/public"),
      STR_INIT("gallery/photosx/new"),
      STR_INIT("gallery/photos/../public/new"),
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(output_dirs) / sizeof(output_dirs[0])); i++) {
    gallery_config.output_dir = output_dirs[i];
    struct Log log = test_log();
    TEST_CHECK(manifest_builder_check_output_dir(&gallery_config, os, test_arena(), &log) == 0);
    TEST_CHECK(test_log_is_empty(&log));
    TEST_MSG("output_dir: '%.*s'", STR_FMT(output_dirs[i]));
  }
}

// Two albums claiming the same output path are rejected with a diagnostic naming the path.
static void test_rejects_duplicate(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  struct Album first_album = {.source_dir = S("a"),
                              .output_path = S("gallery/public/trip/index.html")};
  struct Album second_album = {.source_dir = S("b"),
                               .output_path = S("gallery/public/trip/index.html")};
  const struct Album* albums[] = {&first_album, &second_album};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 2, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'gallery/photos/a' and 'gallery/photos/b': "
                         "'gallery/public/trip/index.html'\n"));
}

// Every collision is reported in one run, in registration order, rather than only the first. A
// rejected claim is not recorded, so the manifest keeps one path from each pair.
static void test_reports_every_collision(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  struct Album album_a = {.source_dir = S("a"), .output_path = S("gallery/public/one/index.html")};
  struct Album album_b = {.source_dir = S("b"), .output_path = S("gallery/public/one/index.html")};
  struct Album album_c = {.source_dir = S("c"), .output_path = S("gallery/public/two/index.html")};
  struct Album album_d = {.source_dir = S("d"), .output_path = S("gallery/public/two/index.html")};
  const struct Album* albums[] = {&album_a, &album_b, &album_c, &album_d};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 4, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'gallery/photos/a' and 'gallery/photos/b': "
                         "'gallery/public/one/index.html'\n"
                         "duplicate output path for 'gallery/photos/c' and 'gallery/photos/d': "
                         "'gallery/public/two/index.html'\n"));
  TEST_CHECK(log.errors == 2);
  TEST_CHECK(manifest.count == 2);
}

// Two albums whose page paths differ only in ASCII case claim one file on a case-insensitive
// filesystem such as the macOS default, where the second write would silently replace the first.
// They are rejected as a duplicate on every platform, so a gallery that builds on Linux does not
// break on macOS. The trailing path is the second claim's own spelling.
static void test_rejects_case_folded_duplicate(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  struct Album first_album = {.source_dir = S("a"),
                              .output_path = S("gallery/public/Trip/index.html")};
  struct Album second_album = {.source_dir = S("b"),
                               .output_path = S("gallery/public/trip/index.html")};
  const struct Album* albums[] = {&first_album, &second_album};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 2, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'gallery/photos/a' and 'gallery/photos/b': "
                         "'gallery/public/trip/index.html'\n"));
}

// Two albums whose page paths collide as a file and a directory, where one is a `/`-delimited
// prefix of the other, are rejected even though neither is a duplicate. The two producers lead the
// message and the path trails. The ancestor path is not repeated because it is a prefix of the path
// that is printed.
static void test_rejects_prefix_collision(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  struct Album file_album = {.source_dir = S("a"), .output_path = S("gallery/public/trip")};
  struct Album nested_album = {.source_dir = S("b"),
                               .output_path = S("gallery/public/trip/index.html")};
  const struct Album* albums[] = {&file_album, &nested_album};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 2, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'gallery/photos/b' nests under output path for "
                         "'gallery/photos/a': 'gallery/public/trip/index.html'\n"));
}

// Every output that nests under another is reported in one run, not only the first.
static void test_rejects_every_prefix_collision(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  static const char* const names[][2] = {{"a", "gallery/public/trip"},
                                         {"b", "gallery/public/trip/index.html"},
                                         {"c", "gallery/public/walk"},
                                         {"d", "gallery/public/walk/index.html"}};
  struct Album albums[4];
  const struct Album* album_list[4];
  for (ptrdiff_t i = 0; i < 4; i++) {
    albums[i] = (struct Album){.source_dir = str_from_cstr(names[i][0]),
                               .output_path = str_from_cstr(names[i][1])};
    album_list[i] = &albums[i];
  }
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, album_list, 4, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'gallery/photos/b' nests under output path for "
                         "'gallery/photos/a': 'gallery/public/trip/index.html'\n"
                         "output path for 'gallery/photos/d' nests under output path for "
                         "'gallery/photos/c': 'gallery/public/walk/index.html'\n"));
  TEST_CHECK(log.errors == 2);
}

// The root album and a configured `index.html` aggregate collide before any writer runs. The root
// is labeled by `input_dir`, a real path, rather than a placeholder a sub-album could share.
static void test_rejects_album_aggregate_collision(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/templates/index.html", "aggregate");
  static const struct Str aggregates[] = {STR_INIT("index.html")};
  gallery_config.aggregate_templates = aggregates;
  gallery_config.aggregate_template_count = 1;
  struct Album album = {.source_dir = S(""), .output_path = S("gallery/public/index.html")};
  const struct Album* albums[] = {&album};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'gallery/photos' and 'aggregate_templates[0]': "
                         "'gallery/public/index.html'\n"));
}

// One name listed twice in `aggregate_templates` is rejected with each entry named by its index
// rather than by its own name. A bare template name reports it as `for 'dup.html' and 'dup.html'`,
// which reads as a value colliding with itself and says nothing about which entry to remove.
static void test_rejects_duplicate_template(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  static const struct Str aggregates[] = {STR_INIT("dup.html"), STR_INIT("dup.html")};
  gallery_config.aggregate_templates = aggregates;
  gallery_config.aggregate_template_count = 2;
  struct Album album = {.source_dir = S(""), .output_path = S("gallery/public/index.html")};
  const struct Album* albums[] = {&album};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'aggregate_templates[0]' and "
                         "'aggregate_templates[1]': 'gallery/public/dup.html'\n"));
}

// `manifest_builder_populate` rejects an output path that names one of the build's own input files,
// so a build cannot overwrite its own source. Here an original output is a hard link to its own
// media source, so the two paths differ and only filesystem identity sees the collision. An output
// inside `input_dir` fails the root check first, as `test_rejects_output_in_input_dir` pins.
static void test_rejects_input_overwrite(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/photos/x.jpg", "source");
  os_memory_link(os, "gallery/photos/x.jpg", "gallery/public/x.jpg");
  struct MediaDerivative derivatives[] = {
      {.name = S("s"), .path = S("gallery/public/small.jpg")},
      {.name = S("l"), .path = S("gallery/public/large.jpg")},
  };
  struct MediaItem media = {.source_path = S("gallery/photos/x.jpg"),
                            .derivatives = derivatives,
                            .derivative_count = 2,
                            .original_path = S("gallery/public/x.jpg")};
  struct MediaItem* media_items[] = {&media};
  struct Album album = {.source_dir = S(""),
                        .output_path = S("gallery/public/index.html"),
                        .media = media_items,
                        .media_count = 1};
  const struct Album* albums[] = {&album};
  struct Arena arena = test_arena();
  struct PathList sources = {0};
  path_list_push(&sources, S("gallery/photos/x.jpg"), &arena);
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, &sources, NULL, albums, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path would overwrite build input for 'gallery/photos/x.jpg': "
                         "'gallery/public/x.jpg'\n"));
}

// The input check finds an aimed-at source wherever it falls among many claimed inputs, and still
// accepts an existing output that is not an input. The sources are created in reverse name order,
// so the order they are claimed in differs from the order their identities sort in. Each aimed-at
// original is a hard link to its source, for the reason `test_rejects_input_overwrite` gives.
static void test_rejects_input_overwrite_among_many_inputs(void) {
  enum { SOURCE_COUNT = 32 };
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/public/existing.jpg", "output");
  struct Arena arena = test_arena();
  struct PathList sources = {0};
  for (ptrdiff_t i = SOURCE_COUNT; i > 0; i--) {
    char source_path[48];
    (void)snprintf(source_path, sizeof(source_path), "gallery/photos/%02td.jpg", i - 1);
    os_memory_write(os, source_path, "source");
  }
  for (ptrdiff_t i = 0; i < SOURCE_COUNT; i++) {
    struct Buf source_path = {.arena = &arena};
    buf_printf(&source_path, "gallery/photos/%02td.jpg", i);
    path_list_push(&sources, buf_str(&source_path), &arena);
  }

  static const ptrdiff_t targets[] = {0, SOURCE_COUNT / 2, SOURCE_COUNT - 1};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(targets) / sizeof(targets[0])); i++) {
    const struct Str target = sources.items[targets[i]];
    struct Buf original = {.arena = &arena};
    buf_printf(&original, "gallery/public/%02td.jpg", targets[i]);
    os_memory_link(os, str_cstr(target, &arena), buf_cstr(&original));
    // The existing output comes first, so a lookup that wrongly matched it would name it instead.
    struct MediaDerivative derivatives[] = {
        {.name = S("s"), .path = S("gallery/public/existing.jpg")}};
    struct MediaItem media = {.source_path = target,
                              .derivatives = derivatives,
                              .derivative_count = 1,
                              .original_path = buf_str(&original)};
    struct MediaItem* media_items[] = {&media};
    struct Album album = {.source_dir = S(""),
                          .output_path = S("gallery/public/index.html"),
                          .media = media_items,
                          .media_count = 1};
    const struct Album* albums[] = {&album};
    struct Manifest manifest = {0};
    struct Log log = test_log();

    TEST_CHECK(populate(&manifest, &gallery_config, &sources, NULL, albums, 1, os, &log) == -1);
    const struct Str original_path = buf_str(&original);
    TEST_CHECK(test_log_is(&log, "output path would overwrite build input for '%.*s': '%.*s'\n",
                           STR_FMT(target), STR_FMT(original_path)));
    TEST_MSG("target %td", targets[i]);
  }
}

// The config file is an input like any other, so an output path naming it is rejected. Without this
// the build would overwrite the file that configured it and still report success: `output_dir` at
// the gallery root plus an aggregate template named `fram.toml` is all it takes, and the loss is
// unrecoverable.
static void test_rejects_config_overwrite(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  static const struct Str aggregates[] = {STR_INIT("fram.toml")};
  gallery_config.output_dir = S("gallery");
  gallery_config.aggregate_templates = aggregates;
  gallery_config.aggregate_template_count = 1;
  struct Album album = {.source_dir = S(""), .output_path = S("gallery/public/index.html")};
  const struct Album* albums[] = {&album};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path would overwrite build input for 'aggregate_templates[0]': "
                         "'gallery/fram.toml'\n"));
}

// Every file below `templates_dir` is a build input, so an output naming the album template or a
// partial is rejected. An output inside `templates_dir` fails the root check first, as
// `test_rejects_output_in_templates_dir` pins, so each output here sits in `output_dir` as a hard
// link to a template, which only the identity claim can see. The partial is named in no
// configuration, so only the walk of the template tree can claim it, which is the case a claim of
// just the configured templates would miss.
static void test_rejects_template_overwrite(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/templates/partials/card.html", "card");
  os_memory_link(os, "gallery/templates/album.html", "gallery/public/index.html");
  os_memory_link(os, "gallery/templates/partials/card.html", "gallery/public/card.html");

  // First case: the album page is a hard link to the configured album template.
  struct Album template_album = {.source_dir = S(""),
                                 .output_path = S("gallery/public/index.html")};
  const struct Album* template_albums[] = {&template_album};
  struct Manifest template_manifest = {0};
  struct Log template_log = test_log();
  TEST_CHECK(populate(&template_manifest, &gallery_config, NULL, NULL, template_albums, 1, os,
                      &template_log) == -1);
  TEST_CHECK(test_log_is(&template_log,
                         "output path would overwrite build input for 'gallery/photos': "
                         "'gallery/public/index.html'\n"));

  // Second case: an aggregate lands on a hard link to an unconfigured partial.
  static const struct Str aggregates[] = {STR_INIT("card.html")};
  gallery_config.aggregate_templates = aggregates;
  gallery_config.aggregate_template_count = 1;
  struct Album partial_album = {.source_dir = S(""), .output_path = S("gallery/public/page.html")};
  const struct Album* partial_albums[] = {&partial_album};
  struct Manifest partial_manifest = {0};
  struct Log partial_log = test_log();
  TEST_CHECK(populate(&partial_manifest, &gallery_config, NULL, NULL, partial_albums, 1, os,
                      &partial_log) == -1);
  TEST_CHECK(test_log_is(&partial_log,
                         "output path would overwrite build input for 'aggregate_templates[0]': "
                         "'gallery/public/card.html'\n"));
}

// An `output_dir` at or below an input root is rejected before anything walks or writes, whether or
// not it exists yet, and the check creates none of it. A missing `output_dir` is judged by the
// directory it would be created in, so a nested path, a `..` past a missing component, and a path
// through a symlinked root all land where `os_mkdirs` would put them.
static void test_check_output_dir_rejects_dir_in_input_root(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/photos/public/old.html", "old");
  os_memory_write(os, "gallery/static/a.css", "static");
  os_memory_symlink(os, "photos", "gallery/photos-link");
  static const struct Str input_output_dirs[] = {
      STR_INIT("gallery/photos"),
      STR_INIT("gallery/photos/public"),
      STR_INIT("gallery/photos/new/deeper"),
      STR_INIT("gallery/missing/../photos/new"),
      STR_INIT("gallery/photos-link/new"),
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(input_output_dirs) / sizeof(input_output_dirs[0]));
       i++) {
    gallery_config.output_dir = input_output_dirs[i];
    check_rejects_output_dir_in_root(&gallery_config, "input_dir", os);
  }

  gallery_config.output_dir = S("gallery/templates/partials");
  check_rejects_output_dir_in_root(&gallery_config, "templates_dir", os);
  gallery_config.output_dir = S("gallery/static/generated");
  check_rejects_output_dir_in_root(&gallery_config, "static_dir", os);
}

// An output that would land below `input_dir` is rejected even though no file sits at its path yet,
// because the next build could discover it as media. The root is compared by identity, so an
// alternate spelling of it, a symlink to it, a symlink inside `output_dir` that leads into it, and
// an `output_dir` inside it are rejected the same way.
static void test_rejects_output_in_input_dir(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/photos/x.jpg", "source");
  os_memory_symlink(os, "photos", "gallery/photos-link");
  os_memory_mkdir(os, "gallery/out");
  os_memory_symlink(os, "../photos", "gallery/out/link");

  // The output is a new file below the root, reached from an `output_dir` that holds the root.
  gallery_config.output_dir = S("gallery");
  check_rejects_output_in_root(&gallery_config, "gallery/photos/b.html", "input_dir", os);

  // The root is configured through a symlink and spelled with a `./` component.
  gallery_config.input_dir = S("gallery/./photos-link");
  check_rejects_output_in_root(&gallery_config, "gallery/photos/sub/b.html", "input_dir", os);

  // A symlink inside `output_dir` leads into the root.
  gallery_config.input_dir = S("gallery/photos");
  gallery_config.output_dir = S("gallery/out");
  check_rejects_output_in_root(&gallery_config, "gallery/out/link/b.html", "input_dir", os);
}

// An output that would land below `templates_dir` is rejected even though no file sits at its path
// yet, because the same build's render could read it back as a partial. The check covers configured
// aggregate outputs as well as album pages.
static void test_rejects_output_in_templates_dir(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  gallery_config.output_dir = S("gallery");
  check_rejects_output_in_root(&gallery_config, "gallery/templates/partials/b.html",
                               "templates_dir", os);

  // A configured aggregate whose name leads into the template tree is rejected by its list entry.
  static const struct Str aggregates[] = {STR_INIT("templates/index.html")};
  gallery_config.aggregate_templates = aggregates;
  gallery_config.aggregate_template_count = 1;
  struct Album album = {.source_dir = S(""), .output_path = S("gallery/public/index.html")};
  const struct Album* albums[] = {&album};
  struct Manifest manifest = {0};
  struct Log log = test_log();
  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path would write inside 'templates_dir' for "
                         "'aggregate_templates[0]': 'gallery/templates/index.html'\n"));
}

// An output that would land below `static_dir` is rejected, because the next build would copy it as
// a static file.
static void test_rejects_output_in_static_dir(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/static/a.css", "static");
  gallery_config.output_dir = S("gallery");
  check_rejects_output_in_root(&gallery_config, "gallery/static/b.html", "static_dir", os);
}

// An output inside an input root is reported once for the album that leads there: a sub-album or a
// media item of that album lands there for the same reason and fails without a line of its own.
// Every other source is reported, in whichever root it lands, because its output may land there for
// a reason of its own, like the static file here.
static void test_reports_input_root_once_per_album(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/photos/x.jpg", "source");
  os_memory_write(os, "gallery/static/link/y.css", "y");
  os_memory_mkdir(os, "gallery/out");
  os_memory_symlink(os, "../photos", "gallery/out/link");
  os_memory_symlink(os, "../templates", "gallery/out/tlink");
  gallery_config.output_dir = S("gallery/out");

  struct Album root = {.source_dir = S(""), .output_path = S("gallery/out/link/index.html")};
  struct Album a = {.source_dir = S("a"), .output_path = S("gallery/out/link/a/index.html")};
  struct Album b = {.source_dir = S("b"), .output_path = S("gallery/out/tlink/b/index.html")};
  struct Album b_sub = {.source_dir = S("b/sub"),
                        .output_path = S("gallery/out/tlink/b/sub/index.html")};
  const struct Album* albums[] = {&root, &a, &b, &b_sub};
  struct Arena arena = test_arena();
  struct PathList static_paths = {0};
  path_list_push(&static_paths, S("gallery/static/link/y.css"), &arena);
  struct Manifest manifest = {0};
  struct Log log = test_log();
  TEST_CHECK(populate(&manifest, &gallery_config, NULL, &static_paths, albums, 4, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path would write inside 'input_dir' for 'gallery/photos': "
                         "'gallery/out/link/index.html'\n"
                         "output path would write inside 'templates_dir' for 'gallery/photos/b': "
                         "'gallery/out/tlink/b/index.html'\n"
                         "output path would write inside 'input_dir' for "
                         "'gallery/static/link/y.css': 'gallery/out/link/y.css'\n"));
  TEST_CHECK(log.errors == 3);
}

// A `templates_dir` that exists but cannot be walked fails the manifest rather than claiming
// nothing, because a partial the walk could not see would stay overwritable. The reason names the
// path the walk failed on, so the configured root is not repeated ahead of it. A regular file in
// place of the directory and a subdirectory that refuses to be listed fail the same way.
static void test_rejects_unlistable_templates_dir(void) {
  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  os_memory_write(os, "gallery/templates-file", "not a directory");
  gallery_config.templates_dir = S("gallery/templates-file");
  struct Album album = {.source_dir = S(""), .output_path = S("gallery/public/index.html")};
  const struct Album* albums[] = {&album};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to list template files: cannot open directory: %s ('gallery/templates-file')\n",
      strerror(ENOTDIR)));
  TEST_CHECK(manifest.count == 0);

  os_memory_write(os, "gallery/templates/partials/card.html", "card");
  os_memory_fail(os, OS_MEMORY_OP_LIST, "gallery/templates/partials", EACCES);
  gallery_config.templates_dir = S("gallery/templates");
  log = test_log();
  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log,
      "failed to list template files: cannot open directory: %s ('gallery/templates/partials')\n",
      strerror(EACCES)));
}

// A configured template whose output path exceeds the whole-path limit is rejected with a message
// naming the limit, the measured length, the list entry, and the whole name.
static void test_rejects_oversize_template_path(void) {
  char name[OUTPUT_PATH_RELATIVE_LEN_MAX + 64];
  memset(name, 'a', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  const struct Str aggregates[] = {{name, (ptrdiff_t)sizeof(name) - 1}};

  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  gallery_config.aggregate_templates = aggregates;
  gallery_config.aggregate_template_count = 1;
  struct Album album = {.source_dir = S(""), .output_path = S("gallery/public/index.html")};
  const struct Album* albums[] = {&album};
  struct Manifest manifest = {0};
  struct Log log = test_log();
  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'aggregate_templates[0]' exceeds max output path length "
                         "(%d bytes) at %td bytes: '%.*s'\n",
                         OUTPUT_PATH_RELATIVE_LEN_MAX, aggregates[0].len, STR_FMT(aggregates[0])));
}

// A configured template with one overlong path segment is rejected against the per-filename limit
// rather than the whole-path one, so the two limits stay distinguishable.
static void test_rejects_oversize_template_segment(void) {
  char name[FILENAME_LEN_MAX + 32];
  memset(name, 'b', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  const struct Str aggregates[] = {{name, (ptrdiff_t)sizeof(name) - 1}};

  struct GalleryConfig gallery_config;
  struct Os* os = init_manifest_fixture(&gallery_config);
  gallery_config.aggregate_templates = aggregates;
  gallery_config.aggregate_template_count = 1;
  struct Album album = {.source_dir = S(""), .output_path = S("gallery/public/index.html")};
  const struct Album* albums[] = {&album};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &gallery_config, NULL, NULL, albums, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path segment for 'aggregate_templates[0]' exceeds max filename "
                         "length (%d bytes) at %zu bytes: '%s'\n",
                         FILENAME_LEN_MAX, sizeof(name) - 1, name));
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
    {"rejects duplicate", test_rejects_duplicate},
    {"reports every collision", test_reports_every_collision},
    {"rejects case folded duplicate", test_rejects_case_folded_duplicate},
    {"rejects prefix collision", test_rejects_prefix_collision},
    {"rejects every prefix collision", test_rejects_every_prefix_collision},
    {"rejects album aggregate collision", test_rejects_album_aggregate_collision},
    {"rejects duplicate template", test_rejects_duplicate_template},
    {"rejects input overwrite", test_rejects_input_overwrite},
    {"rejects input overwrite among many inputs", test_rejects_input_overwrite_among_many_inputs},
    {"rejects config overwrite", test_rejects_config_overwrite},
    {"rejects template overwrite", test_rejects_template_overwrite},
    {"check output dir rejects dir in input root", test_check_output_dir_rejects_dir_in_input_root},
    {"rejects output in input dir", test_rejects_output_in_input_dir},
    {"rejects output in templates dir", test_rejects_output_in_templates_dir},
    {"rejects output in static dir", test_rejects_output_in_static_dir},
    {"reports input root once per album", test_reports_input_root_once_per_album},
    {"rejects unlistable templates dir", test_rejects_unlistable_templates_dir},
    {"rejects oversize template path", test_rejects_oversize_template_path},
    {"rejects oversize template segment", test_rejects_oversize_template_segment},
    {NULL, NULL},
};
