#include <acutest.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "build/output_writer.h"
#include "build/page_renderer.h"
#include "build/site_writer.h"
#include "core/path_list.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/manifest.h"
#include "os_memory.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// Every test here runs against the in-memory host: the writers read templates and static files and
// write their outputs only through `runtime/os.h`.

/**
 * @brief Initializes a writer on `os` whose manifest names `output_paths`, as the build's would.
 *
 * @param writer            Writer to initialize. Must not be `NULL`.
 * @param manifest          Empty manifest that receives the paths. Must not be `NULL`.
 * @param output_paths      Array of `output_path_count` terminated paths. Must not be `NULL`.
 * @param output_path_count Number of paths in `output_paths`.
 * @param os                Host the writer writes to. Must not be `NULL`.
 * @param arena             Arena that owns the manifest's entries. Must not be `NULL`.
 */
static void init_writer(struct OutputWriter* writer,
                        struct Manifest* manifest,
                        const char* const* output_paths,
                        ptrdiff_t output_path_count,
                        struct Os* os,
                        struct Arena* arena) {
  for (ptrdiff_t i = 0; i < output_path_count; i++) {
    TEST_CHECK(manifest_add(manifest, str_from_cstr(output_paths[i]), S("test"), arena, NULL) == 0);
  }
  *writer = (struct OutputWriter){.manifest = manifest, .os = os};
}

// Album pages, aggregate templates, and nested static files land at their planned destinations.
static void test_writes_complete_output_layer(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/map.xml", "<g>{{gallery.title}}</g>");
  os_memory_write(os, "templates/nested/links.html", "{{album.url}}");
  os_memory_write(os, "static/icons/a.txt", "static");
  struct Arena paths = test_arena();
  const struct Arena output = test_arena();
  const struct Arena scratch = test_arena();
  static const struct Str aggregates[] = {STR_INIT("map.xml"), STR_INIT("nested/links.html")};
  struct GalleryConfig gallery_config;
  gallery_config_init(&gallery_config);
  gallery_config.title = S("Gallery & Co");
  gallery_config.author = S("Ada");
  gallery_config.output_dir = S("public");
  gallery_config.templates_dir = S("templates");
  gallery_config.static_dir = S("static");
  gallery_config.aggregate_templates = aggregates;
  gallery_config.aggregate_template_count = 2;
  struct Album root = {.source_dir = S(""),
                       .title = S("Root"),
                       .url_path = S("index.html"),
                       .output_path = S("public/index.html"),
                       .path_to_root = S("")};
  const struct Album* albums[] = {&root};
  const struct RenderedPage rendered[] = {{.html = S("<h1>Root</h1>")}};
  struct PathList static_paths = {0};
  struct Log log = test_log();
  path_list_push(&static_paths, S("static/icons/a.txt"), &paths);
  static const char* const output_paths[] = {"public/index.html", "public/map.xml",
                                             "public/nested/links.html", "public/icons/a.txt"};
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 4, os, &paths);

  TEST_CHECK(site_writer_write_album_pages(&writer, albums, 1, rendered, scratch, &log) == 0);
  TEST_CHECK(site_writer_write_aggregates(&writer, &gallery_config, albums[0], os, output, scratch,
                                          &log) == 0);
  TEST_CHECK(
      site_writer_copy_static_files(&writer, &gallery_config, &static_paths, scratch, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/index.html", "<h1>Root</h1>"));
  TEST_CHECK(os_memory_is_file(os, "public/map.xml", "<g>Gallery &amp; Co</g>"));
  TEST_CHECK(os_memory_is_file(os, "public/nested/links.html", "../index.html"));
  TEST_CHECK(os_memory_is_file(os, "public/icons/a.txt", "static"));
}

// A failed page write names the root album in words rather than quoting its empty source directory,
// then the cause and the destination. The destination is an existing directory, so opening it for
// writing fails.
static void test_write_album_pages_names_root_album_in_words(void) {
  struct Os* os = os_memory();
  os_memory_mkdir(os, "public");
  struct Album root = {.source_dir = S(""), .title = S("Root"), .output_path = S("public")};
  const struct Album* albums[] = {&root};
  const struct RenderedPage rendered[] = {{.html = S("<h1>Root</h1>")}};
  static const char* const output_paths[] = {"public"};
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 1, os, &arena);
  struct Log log = test_log();

  TEST_CHECK(site_writer_write_album_pages(&writer, albums, 1, rendered, test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write output for the root album: %s ('public')\n",
                         strerror(EISDIR)));
}

// A failed sub-album page write quotes the album's source directory, even one literally named
// `root album`, so it never reads the same as the root's diagnostic.
static void test_write_album_pages_quotes_sub_album_source_dir(void) {
  struct Os* os = os_memory();
  os_memory_mkdir(os, "public");
  struct Album album = {
      .source_dir = S("root album"), .title = S("root album"), .output_path = S("public")};
  const struct Album* albums[] = {&album};
  const struct RenderedPage rendered[] = {{.html = S("<h1>root album</h1>")}};
  static const char* const output_paths[] = {"public"};
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 1, os, &arena);
  struct Log log = test_log();

  TEST_CHECK(site_writer_write_album_pages(&writer, albums, 1, rendered, test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write output for 'root album': %s ('public')\n",
                         strerror(EISDIR)));
}

// A failed aggregate write names the operation and the cause, then the destination. The destination
// is an existing directory, so opening it for writing fails.
static void test_write_aggregates_reports_write_failure(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/map.xml", "<g></g>");
  os_memory_mkdir(os, "public/map.xml");
  static const struct Str aggregates[] = {STR_INIT("map.xml")};
  struct GalleryConfig gallery_config;
  gallery_config_init(&gallery_config);
  gallery_config.output_dir = S("public");
  gallery_config.templates_dir = S("templates");
  gallery_config.aggregate_templates = aggregates;
  gallery_config.aggregate_template_count = 1;
  struct Album root = {.source_dir = S(""), .title = S("Root"), .path_to_root = S("")};
  static const char* const output_paths[] = {"public/map.xml"};
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 1, os, &arena);
  struct Log log = test_log();

  TEST_CHECK(site_writer_write_aggregates(&writer, &gallery_config, &root, os, test_arena(),
                                          test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write template output: %s ('public/map.xml')\n",
                         strerror(EISDIR)));
}

// A failed static copy names the operation and the cause, then the path that failed: the
// destination when it is an existing directory, and the static file when it is gone.
static void test_copy_static_files_reports_copy_failure(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "static/a.css", "a {}");
  os_memory_mkdir(os, "public/a.css");
  struct GalleryConfig gallery_config;
  gallery_config_init(&gallery_config);
  gallery_config.output_dir = S("public");
  gallery_config.static_dir = S("static");
  struct Arena paths = test_arena();
  struct PathList static_paths = {0};
  path_list_push(&static_paths, S("static/a.css"), &paths);
  static const char* const output_paths[] = {"public/a.css", "public/gone.css"};
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 2, os, &paths);
  struct Log log = test_log();

  TEST_CHECK(site_writer_copy_static_files(&writer, &gallery_config, &static_paths, test_arena(),
                                           &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "failed to copy static file: %s ('public/a.css')\n", strerror(EISDIR)));

  static_paths = (struct PathList){0};
  path_list_push(&static_paths, S("static/gone.css"), &paths);
  log = test_log();
  TEST_CHECK(site_writer_copy_static_files(&writer, &gallery_config, &static_paths, test_arena(),
                                           &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "failed to copy static file: %s ('static/gone.css')\n", strerror(ENOENT)));
}

TEST_LIST = {
    {"writes complete output layer", test_writes_complete_output_layer},
    {"write album pages names root album in words",
     test_write_album_pages_names_root_album_in_words},
    {"write album pages quotes sub album source dir",
     test_write_album_pages_quotes_sub_album_source_dir},
    {"write aggregates reports write failure", test_write_aggregates_reports_write_failure},
    {"copy static files reports copy failure", test_copy_static_files_reports_copy_failure},
    {NULL, NULL},
};
