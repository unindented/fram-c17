#ifndef FRAM_PAGE_RENDERER_H
#define FRAM_PAGE_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct Album;
struct JobArenas;
struct Log;
struct GalleryConfig;
struct Os;

/** One album's rendered page, the slot `page_renderer_render_pages` fills. */
struct RenderedPage {
  /**
   * Page HTML in the perm of the worker that rendered it, or empty until a render fills the slot.
   */
  struct Str html;
};

/**
 * @brief Renders each album's page through the configured album template in parallel.
 *
 * This is the third of the three parallel phases. When `album_count` is 0, it runs no job and
 * returns `0`. One job owns each album and renders it with that album as `album_current`, so each
 * page resolves its URLs against its own `path_to_root`. A job renders its page into its worker's
 * `perm`, where the page stays until the write phase, and its temporaries into a copy of its
 * worker's scratch. A failed render leaves the worker's `perm` as it found it. Each finished job
 * reports progress when `is_verbose`. Failing jobs' errors go to `log` in album order, as `job_run`
 * collects them.
 *
 * @param gallery_config Configuration supplying `templates_dir` and the album template name. Must
 *                       not be `NULL`.
 * @param albums         Albums to render, one job per element. Read-only during the run. Must not
 *                       be `NULL`.
 * @param album_count    Number of albums in `albums`, and of slots in `rendered_pages`.
 * @param worker_count   Requested worker threads, as for `pool_run`.
 * @param worker_arenas  One `JobArenas` per requested worker, as for `job_run`. Each worker's
 *                       `perm` receives the pages it renders, so it must outlive the pages' write.
 *                       Must not be `NULL`.
 * @param is_verbose     Whether progress is printed to `stderr`.
 * @param os             Host the templates are read from and progress printed on, by every worker
 *                       at once. Must not be `NULL`.
 * @param scratch        Arena for the phase's own bookkeeping, as for `job_run`.
 * @param rendered_pages One slot per album, each zero-initialized by the caller. A successful job
 *                       stores its page there. A failed job leaves its slot's `html` empty. Must
 *                       not be `NULL`.
 * @param log            Log that receives the collected render errors. Its arena must be none of
 *                       the others. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a render job failed or the worker pool could
 *         not start.
 */
int page_renderer_render_pages(const struct GalleryConfig* gallery_config,
                               const struct Album* const* albums,
                               ptrdiff_t album_count,
                               ptrdiff_t worker_count,
                               struct JobArenas* worker_arenas,
                               bool is_verbose,
                               struct Os* os,
                               struct Arena scratch,
                               struct RenderedPage* rendered_pages,
                               struct Log* log) __attribute__((nonnull(1, 2, 5, 7, 9, 10)));

#endif
