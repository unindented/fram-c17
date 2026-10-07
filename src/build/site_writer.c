#include "build/site_writer.h"

#include <stdlib.h>

#include "build/manifest_builder.h"
#include "build/output_path.h"
#include "build/page_renderer.h"
#include "build/template.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "runtime/fs.h"
#include "shared/arena.h"

// This module's three exported writers are unit-tested in `src/build/test_site_writer.c`, and
// CTest's golden test suite exercises them end to end by building every fixture gallery and
// comparing each result with its `tests/expected/<gallery>` tree. Collision checks live in
// `manifest_builder`, unit-tested in `src/build/test_manifest_builder.c`.

/**
 * @brief Renders one configured template against the root album and writes it to its matching
 *        output path.
 *
 * @param gallery_config Configuration supplying `templates_dir` and `output_dir`. Must not be
 *                       `NULL`.
 * @param root_album     Root album whose data the template renders against. Must not be `NULL`.
 * @param template_name  Safe relative template name to render. `manifest_builder_populate` already
 *                       checked it against the output-path limits, because every call site runs the
 *                       manifest phase over the same configured list first. This function does not
 *                       re-check, so an overlong name reaching here fails as an opaque write error
 *                       from the OS rather than with a diagnostic naming the limit. Must not be
 *                       `NULL`.
 * @param err            Destination buffer for a failure diagnostic.
 * @param err_len        Size of `err` in bytes.
 * @return `0` on success, or `-1` on a URL-context, render, path, or write failure.
 */
static int write_rendered_template(const struct GalleryConfig* gallery_config,
                                   const struct Album* root_album,
                                   const char* template_name,
                                   char* err,
                                   size_t err_len) __attribute__((nonnull(1, 2, 3)));

int site_writer_write_album_pages(const struct Album* const* albums,
                                  size_t album_count,
                                  const struct RenderedPage* rendered_pages,
                                  char* err,
                                  size_t err_len) {
  for (size_t i = 0; i < album_count; i++) {
    char reason[FS_REASON_SIZE];
    if (fs_write_file(albums[i]->output_path, rendered_pages[i].html, rendered_pages[i].html_len,
                      reason, sizeof(reason)) == 0) {
      continue;
    }
    if (album_is_root(albums[i])) {
      return error_report(err, err_len, "failed to write output: %s (for the root album, to '%s')",
                          reason, albums[i]->output_path);
    }
    return error_report(err, err_len, "failed to write output: %s (for '%s', to '%s')", reason,
                        albums[i]->source_dir, albums[i]->output_path);
  }
  return 0;
}

int site_writer_write_aggregates(const struct GalleryConfig* gallery_config,
                                 const struct Album* root_album,
                                 char* err,
                                 size_t err_len) {
  for (size_t i = 0; i < gallery_config->aggregate_template_count; i++) {
    if (write_rendered_template(gallery_config, root_album, gallery_config->aggregate_templates[i],
                                err, err_len) != 0) {
      return -1;
    }
  }
  return 0;
}

int site_writer_copy_static_files(const struct GalleryConfig* gallery_config,
                                  const struct PathList* static_paths,
                                  char* err,
                                  size_t err_len) {
  struct Arena scratch;
  arena_init(&scratch);
  int rc = 0;
  for (size_t i = 0; rc == 0 && i < static_paths->count; i++) {
    const char* relative = path_relative_below(static_paths->items[i], gallery_config->static_dir);
    char* output_path =
        manifest_builder_derive_static_output(gallery_config->output_dir, relative, &scratch);
    if (output_path == NULL) {
      (void)error_report(err, err_len, "out of memory building output path for static file '%s'",
                         static_paths->items[i]);
      rc = -1;
      continue;
    }
    char reason[FS_REASON_SIZE];
    if (fs_copy_file(static_paths->items[i], output_path, reason, sizeof(reason)) != 0) {
      (void)error_report(err, err_len, "failed to copy static file: %s (for '%s', to '%s')", reason,
                         static_paths->items[i], output_path);
      rc = -1;
    }
  }
  arena_free(&scratch);
  return rc;
}

static int write_rendered_template(const struct GalleryConfig* gallery_config,
                                   const struct Album* root_album,
                                   const char* template_name,
                                   char* err,
                                   size_t err_len) {
  struct Arena scratch;
  arena_init(&scratch);

  int rc = -1;
  char* output_path = NULL;
  size_t rendered_html_len = 0;
  char* rendered_html = NULL;
  // An aggregate renders against the root album, whose own `path_to_root` is empty, so a nested
  // aggregate such as `feeds/map.xml` needs its own prefix for page-relative URLs to resolve.
  struct Album aggregate_album = *root_album;
  const struct TemplateContext context = {
      .gallery_config = gallery_config,
      .album_current = &aggregate_album,
  };
  aggregate_album.path_to_root = output_path_to_root(template_name, &scratch);
  if (aggregate_album.path_to_root == NULL) {
    (void)error_report(err, err_len, "out of memory building aggregate URL context for '%s'",
                       template_name);
    goto cleanup;
  }

  rendered_html = template_render_file(gallery_config->templates_dir, template_name, &context,
                                       &rendered_html_len, err, err_len);
  if (rendered_html == NULL) {
    goto cleanup;
  }

  output_path =
      manifest_builder_derive_template_output(gallery_config->output_dir, template_name, &scratch);
  if (output_path == NULL) {
    (void)error_report(err, err_len, "out of memory building output path for '%s'", template_name);
    goto cleanup;
  }
  char reason[FS_REASON_SIZE];
  if (fs_write_file(output_path, rendered_html, rendered_html_len, reason, sizeof(reason)) != 0) {
    (void)error_report(err, err_len, "failed to write template output: %s ('%s')", reason,
                       output_path);
    goto cleanup;
  }
  rc = 0;

cleanup:
  arena_free(&scratch);
  free(rendered_html);
  return rc;
}
