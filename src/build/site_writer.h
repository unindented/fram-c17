#ifndef FRAM_SITE_WRITER_H
#define FRAM_SITE_WRITER_H

#include <stddef.h>

struct Album;
struct GalleryConfig;
struct PathList;
struct RenderedPage;

// These are declared in the order `cmd_build` runs them. The manifest phase in `manifest_builder`
// claims every intended output path before this module writes anything.

/**
 * @brief Writes each already-rendered album page to its planned output path.
 *
 * Call this only once `page_renderer_render_pages` returns `0`, which guarantees that every
 * `rendered_pages[i].html` is non-`NULL`. A `NULL` page is a caller error, not a skipped album.
 *
 * @param albums         Albums whose `output_path` receives the matching page. Must not be `NULL`.
 * @param album_count    Number of albums in `albums`, and of slots in `rendered_pages`.
 * @param rendered_pages Rendered page per album, parallel to `albums`, each written as its
 *                       `html_len` bytes. Every `html` must be non-`NULL`. Borrowed. Must not be
 *                       `NULL`.
 * @param err            Destination buffer for a failure diagnostic.
 * @param err_len        Size of `err` in bytes.
 * @return `0` on success, or `-1` on the first write failure.
 */
int site_writer_write_album_pages(const struct Album* const* albums,
                                  size_t album_count,
                                  const struct RenderedPage* rendered_pages,
                                  char* err,
                                  size_t err_len) __attribute__((nonnull(1, 3)));

/**
 * @brief Renders and writes each configured aggregate template below the output directory.
 *
 * Each aggregate renders with the root album's data as `album_current`, but with `path_to_root`
 * rebased onto the aggregate's own output depth, so a page-relative URL resolves from the directory
 * the aggregate is written to.
 *
 * @param gallery_config Configuration supplying `output_dir`, `templates_dir`, and the aggregate
 *                       template list. Must not be `NULL`.
 * @param root_album     Root album whose data every aggregate renders against. Must not be `NULL`.
 * @param err            Destination buffer for a failure diagnostic.
 * @param err_len        Size of `err` in bytes.
 * @return `0` on success, including when no aggregate is configured, or `-1` on the first template
 *         that fails to render or write.
 */
int site_writer_write_aggregates(const struct GalleryConfig* gallery_config,
                                 const struct Album* root_album,
                                 char* err,
                                 size_t err_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Copies every enumerated static file below `output_dir`.
 *
 * Each static file keeps its path relative to `static_dir`. `manifest_builder_derive_static_output`
 * derives the destination, as it did for the path `manifest_builder_populate` registered.
 *
 * @param gallery_config Configuration supplying `output_dir` and `static_dir`. Must not be `NULL`.
 * @param static_paths   Every enumerated file below `static_dir`, each already accepted by
 *                       `manifest_builder_populate`, which rejects a path outside `static_dir`.
 *                       Must not be `NULL`.
 * @param err            Destination buffer for a failure diagnostic.
 * @param err_len        Size of `err` in bytes.
 * @return `0` on success, or `-1` on the first allocation or copy failure.
 */
int site_writer_copy_static_files(const struct GalleryConfig* gallery_config,
                                  const struct PathList* static_paths,
                                  char* err,
                                  size_t err_len) __attribute__((nonnull(1, 2)));

#endif
