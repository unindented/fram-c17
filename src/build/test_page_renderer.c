#include <acutest.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "build/job.h"
#include "build/page_renderer.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "os_memory.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// Every test here runs against the in-memory host: the page phase reads templates only through
// `runtime/os.h`.

// Parallel jobs render each album into its corresponding output slot with independent context.
static void test_renders_pages_by_index(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/album.html", "{{album.title}}:{{path_to_root}}");
  struct GalleryConfig gallery_config;
  gallery_config_init(&gallery_config);
  gallery_config.title = S("Gallery");
  gallery_config.author = S("Ada");
  struct Album root = {.source_dir = S(""), .title = S("Root"), .path_to_root = S("")};
  struct Album child = {.source_dir = S("trip"), .title = S("Trip"), .path_to_root = S("../")};
  const struct Album* albums[] = {&root, &child};
  struct RenderedPage rendered[2] = {0};
  struct Arena arena = test_arena();
  struct Log log = {.buf = {.arena = &arena}};
  // Every worker gets arenas of its own, as `cmd_build_execute` carves them, so no two concurrent
  // jobs share one.
  struct JobArenas worker_arenas[] = {{.perm = test_arena(), .scratch = test_arena()},
                                      {.perm = test_arena(), .scratch = test_arena()}};

  TEST_CHECK(page_renderer_render_pages(&gallery_config, albums, 2, 2, worker_arenas, false, os,
                                        test_arena(), rendered, &log) == 0);
  TEST_CHECK(str_eq(rendered[0].html, S("Root:")));
  TEST_CHECK(str_eq(rendered[1].html, S("Trip:../")));
  TEST_CHECK(test_log_is_empty(&log));
}

// A missing album template surfaces a per-album error in the collected log. The root album is named
// in words rather than by its empty source directory.
static void test_reports_missing_template(void) {
  struct GalleryConfig gallery_config;
  gallery_config_init(&gallery_config);
  gallery_config.title = S("Gallery");
  gallery_config.author = S("Ada");
  gallery_config.templates_dir = S("/does/not/exist");
  struct Album root = {.source_dir = S(""), .title = S("Root"), .path_to_root = S("")};
  const struct Album* albums[] = {&root};
  struct RenderedPage rendered[1] = {0};
  struct Arena arena = test_arena();
  struct Log log = {.buf = {.arena = &arena}};
  struct JobArenas worker_arenas = {.perm = test_arena(), .scratch = test_arena()};

  TEST_CHECK(page_renderer_render_pages(&gallery_config, albums, 1, 1, &worker_arenas, false,
                                        os_memory(), test_arena(), rendered, &log) == -1);
  TEST_CHECK(rendered[0].html.len == 0);
  // The error names the template file that could not be read and why, then the failing album.
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(
      test_log_is(&log,
                  "failed to read template: %s ('/does/not/exist/%.*s') (while rendering the "
                  "root album)\n",
                  strerror(ENOENT), STR_FMT(gallery_config.album_template)));
}

// A missing album template reported for a sub-album quotes its source directory, even one literally
// named `root album`, so it never reads the same as the root's attribution.
static void test_reports_missing_template_for_sub_album(void) {
  struct GalleryConfig gallery_config;
  gallery_config_init(&gallery_config);
  gallery_config.title = S("Gallery");
  gallery_config.author = S("Ada");
  gallery_config.templates_dir = S("/does/not/exist");
  struct Album album = {
      .source_dir = S("root album"), .title = S("root album"), .path_to_root = S("../")};
  const struct Album* albums[] = {&album};
  struct RenderedPage rendered[1] = {0};
  struct Arena arena = test_arena();
  struct Log log = {.buf = {.arena = &arena}};
  struct JobArenas worker_arenas = {.perm = test_arena(), .scratch = test_arena()};

  TEST_CHECK(page_renderer_render_pages(&gallery_config, albums, 1, 1, &worker_arenas, false,
                                        os_memory(), test_arena(), rendered, &log) == -1);
  TEST_CHECK(rendered[0].html.len == 0);
  // The error names the template file that could not be read and why, then the failing album.
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log,
                         "failed to read template: %s ('/does/not/exist/%.*s') (while rendering "
                         "album 'root album')\n",
                         strerror(ENOENT), STR_FMT(gallery_config.album_template)));
}

TEST_LIST = {
    {"renders pages by index", test_renders_pages_by_index},
    {"reports missing template", test_reports_missing_template},
    {"reports missing template for sub album", test_reports_missing_template_for_sub_album},
    {NULL, NULL},
};
