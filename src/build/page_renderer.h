#ifndef FRAM_PAGE_RENDERER_H
#define FRAM_PAGE_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

struct Album;
struct GalleryConfig;
struct StringBuffer;

/** One album's rendered page, the slot `page_renderer_render_pages` fills. */
struct RenderedPage {
  /** Terminated page HTML the owner must `free`, or `NULL` until a render fills the slot. */
  char* html;

  /** Length of `html` in bytes, excluding its terminator. */
  size_t html_len;
};

/**
 * @brief Renders each album's page through the configured album template in parallel.
 *
 * This is the third of the three parallel phases. When `album_count` is 0, it runs no job and
 * returns `0`. One job owns each album and renders it with that album as `album_current`, so each
 * page resolves its URLs against its own `path_to_root`. Each finished job reports progress when
 * `is_verbose`. Failing jobs' diagnostics go to `error_out` one per line, each distinct message
 * once, up to `JOB_ERROR_REPORT_COUNT_MAX` of them plus a count of the rest, so the caller reports
 * them at a single boundary.
 *
 * @param gallery_config Configuration supplying `templates_dir` and the album template name. Must
 *                       not be `NULL`.
 * @param albums         Albums to render, one job per element. Read-only during the run. Must not
 *                       be `NULL`.
 * @param album_count    Number of albums in `albums`, and of slots in `rendered_pages`.
 * @param worker_count   Requested worker threads, as for `pool_run`.
 * @param is_verbose     Whether progress is printed to `stderr`.
 * @param rendered_pages One slot per album, each zero-initialized by the caller. A successful job
 *                       stores its page and length there, and the caller must `free` the page,
 *                       including when another job failed. A failed job leaves its slot's `html`
 *                       `NULL`. Must not be `NULL`.
 * @param error_out      Growable buffer that receives the collected render diagnostics. Must not be
 *                       `NULL`.
 * @return `0` when every job succeeded, or `-1` when a render job failed, the error slots could not
 *         be allocated, the worker pool could not start, or a diagnostic could not be appended.
 */
int page_renderer_render_pages(const struct GalleryConfig* gallery_config,
                               const struct Album* const* albums,
                               size_t album_count,
                               size_t worker_count,
                               bool is_verbose,
                               struct RenderedPage* rendered_pages,
                               struct StringBuffer* error_out) __attribute__((nonnull(1, 2, 6, 7)));

#endif
