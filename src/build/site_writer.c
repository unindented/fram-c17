#include "build/site_writer.h"

#include <stddef.h>

#include "build/output_path.h"
#include "build/output_writer.h"
#include "build/page_renderer.h"
#include "build/template.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "runtime/memory.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"

// This module's three exported writers are unit-tested in `src/build/test_site_writer.c`, and
// CTest's golden test suite exercises them end to end by building every fixture gallery and
// comparing each result with its `tests/expected/<gallery>` tree. Collision checks live in
// `manifest_builder`, unit-tested in `src/build/test_manifest_builder.c`.

/**
 * @brief Renders one configured template against the root album and writes it to its matching
 *        output path.
 *
 * @param writer         Writer the output is written through. Must not be `NULL`.
 * @param gallery_config Configuration supplying `templates_dir` and `output_dir`. Must not be
 *                       `NULL`.
 * @param root_album     Root album whose data the template renders against. Must not be `NULL`.
 * @param template_name  Safe relative template name to render. `manifest_builder_populate` already
 *                       checked it against the output-path limits, because every call site runs the
 *                       manifest phase over the same configured list first. This function does not
 *                       re-check, so an overlong name reaching here fails as an opaque write error
 *                       from the OS rather than with a diagnostic naming the limit.
 * @param os             Host the template is read from. Must not be `NULL`.
 * @param output         Arena that holds the rendered output until it is written.
 * @param scratch        Arena for the render's temporaries, the URL context and the output path.
 * @param log            Log that receives the first failure. Its arena must be none of the others.
 *                       Must not be `NULL`.
 * @return `0` on success, or `-1` on a render or write failure.
 */
static int write_rendered_template(const struct OutputWriter* writer,
                                   const struct GalleryConfig* gallery_config,
                                   const struct Album* root_album,
                                   struct Str template_name,
                                   struct Os* os,
                                   struct Arena output,
                                   struct Arena scratch,
                                   struct Log* log) __attribute__((nonnull(1, 2, 3, 5, 8)));

int site_writer_write_album_pages(const struct OutputWriter* writer,
                                  const struct Album* const* albums,
                                  ptrdiff_t album_count,
                                  const struct RenderedPage* rendered_pages,
                                  struct Arena scratch,
                                  struct Log* log) {
  for (ptrdiff_t i = 0; i < album_count; i++) {
    memory_oom_set_job(albums[i]->output_path, NULL);
    // The operation names the album, and the write names the output path or the directory it could
    // not create, so a failure reads `failed to write output for '<album>': <cause> ('<path>')`.
    // Each page's operation lives in a copy of `scratch`, which the next page reuses.
    struct Arena page_scratch = scratch;
    struct Buf operation = {.arena = &page_scratch};
    if (album_is_root(albums[i])) {
      buf_printf(&operation, "failed to write output for the root album");
    } else {
      buf_printf(&operation, "failed to write output for '%.*s'", STR_FMT(albums[i]->source_dir));
    }
    if (output_writer_write(writer, albums[i]->output_path,
                            (struct OsSource){.bytes = rendered_pages[i].html}, buf_str(&operation),
                            log) != 0) {
      return -1;
    }
  }
  return 0;
}

int site_writer_write_aggregates(const struct OutputWriter* writer,
                                 const struct GalleryConfig* gallery_config,
                                 const struct Album* root_album,
                                 struct Os* os,
                                 struct Arena output,
                                 struct Arena scratch,
                                 struct Log* log) {
  for (ptrdiff_t i = 0; i < gallery_config->aggregate_template_count; i++) {
    if (write_rendered_template(writer, gallery_config, root_album,
                                gallery_config->aggregate_templates[i], os, output, scratch,
                                log) != 0) {
      return -1;
    }
  }
  return 0;
}

int site_writer_copy_static_files(const struct OutputWriter* writer,
                                  const struct GalleryConfig* gallery_config,
                                  const struct PathList* static_paths,
                                  struct Arena scratch,
                                  struct Log* log) {
  for (ptrdiff_t i = 0; i < static_paths->count; i++) {
    memory_oom_set_job(static_paths->items[i], NULL);
    // Each copy works in its own copy of `scratch`, so one file's path is dead before the next.
    struct Arena copy_scratch = scratch;
    struct Str relative = {0};
    (void)path_is_below(static_paths->items[i], gallery_config->static_dir, &relative);
    const struct Str output_path = path_join(gallery_config->output_dir, relative, &copy_scratch);
    // The copy names the path that failed, the static file or its destination, which mirrors it.
    const struct OsSource source = {.kind = OS_SOURCE_FILE, .file_path = static_paths->items[i]};
    if (output_writer_write(writer, output_path, source, S("failed to copy static file"), log) !=
        0) {
      return -1;
    }
  }
  return 0;
}

static int write_rendered_template(const struct OutputWriter* writer,
                                   const struct GalleryConfig* gallery_config,
                                   const struct Album* root_album,
                                   struct Str template_name,
                                   struct Os* os,
                                   struct Arena output,
                                   struct Arena scratch,
                                   struct Log* log) {
  memory_oom_set_job(template_name, NULL);
  // An aggregate renders against the root album, whose own `path_to_root` is empty, so a nested
  // aggregate such as `feeds/map.xml` needs its own prefix for page-relative URLs to resolve.
  struct Album aggregate_album = *root_album;
  aggregate_album.path_to_root = output_path_to_root(template_name, &scratch);
  const struct TemplateContext context = {
      .gallery_config = gallery_config,
      .album_current = &aggregate_album,
  };
  struct Str rendered_html = {0};
  if (template_render_file(gallery_config->templates_dir, template_name, &context, os, &output,
                           scratch, &rendered_html, log) != 0) {
    return -1;
  }
  // The render has returned, so its temporaries in `scratch` are dead and the path can reuse them.
  const struct Str output_path = path_join(gallery_config->output_dir, template_name, &scratch);
  return output_writer_write(writer, output_path, (struct OsSource){.bytes = rendered_html},
                             S("failed to write template output"), log);
}
