#include "build/page_renderer.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/job.h"
#include "build/template.h"
#include "core/path.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "runtime/memory.h"
#include "shared/arena.h"
#include "shared/str.h"

/**
 * Worker context shared by all album page render jobs. Every field is read-only to a job, except
 * `rendered_pages`, where each job writes its own slot.
 */
struct PageRenderContext {
  /** Configuration supplying the template directory and the album template name. */
  const struct GalleryConfig* gallery_config;

  /** Albums to render, one per job index. No album is written during the run. */
  const struct Album* const* albums;

  /** Host every job reads its templates from. */
  struct Os* os;

  /** One rendered-page slot per album. A job writes only `rendered_pages[index]`. */
  struct RenderedPage* rendered_pages;
};

/**
 * @brief Renders one album page into its result slot.
 *
 * This is the `JobFn` of the page phase, so it runs concurrently with other indexes and writes only
 * `rendered_pages[index]`, its own log and its worker's arenas. The HTML goes into the worker's
 * `perm`, where it stays until the write phase. A failure is logged with the album named, leaves
 * the slot's `html` empty, and leaves the worker's `perm` as it found it.
 *
 * @param index    Album index this job handles. Must be below the album count.
 * @param arenas   Arenas of the worker running the job. Must not be `NULL`.
 * @param log      Log of the job, which receives the failure. Must not be `NULL`.
 * @param userdata `struct PageRenderContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording the failure.
 */
static int render_album_page_job(ptrdiff_t index,
                                 struct JobArenas* arenas,
                                 struct Log* log,
                                 void* userdata) __attribute__((nonnull(2, 3, 4)));

int page_renderer_render_pages(const struct GalleryConfig* gallery_config,
                               const struct Album* const* albums,
                               ptrdiff_t album_count,
                               ptrdiff_t worker_count,
                               struct JobArenas* worker_arenas,
                               bool is_verbose,
                               struct Os* os,
                               struct Arena scratch,
                               struct RenderedPage* rendered_pages,
                               struct Log* log) {
  struct PageRenderContext context = {
      .gallery_config = gallery_config,
      .albums = albums,
      .os = os,
      .rendered_pages = rendered_pages,
  };
  return job_run(album_count, worker_count, worker_arenas, render_album_page_job, &context,
                 S("rendering albums"), S(TEMPLATE_RENDER_ATTRIBUTION), is_verbose, os, scratch,
                 log);
}

static int render_album_page_job(ptrdiff_t index,
                                 struct JobArenas* arenas,
                                 struct Log* log,
                                 void* userdata) {
  const struct PageRenderContext* context = userdata;
  const struct Album* album = context->albums[index];
  // The report names the album's source directory, as the other jobs name their source. A nested
  // album's path is joined in the job's scratch, and the render allocates after it.
  struct Arena scratch = arenas->scratch;
  const struct Str input_dir = context->gallery_config->input_dir;
  memory_oom_set_job(
      album_is_root(album) ? input_dir : path_join(input_dir, album->source_dir, &scratch), log);
  const struct TemplateContext template_context = {
      .gallery_config = context->gallery_config,
      .album_current = album,
      .is_album_page = true,
  };
  struct RenderedPage* page = &context->rendered_pages[index];
  // The page renders into a copy of the worker's `perm`, which is committed only when the render
  // succeeds, so a failed render leaves nothing behind for the write phase.
  struct Arena perm = arenas->perm;
  // The render names the album in its failure, because `is_album_page` is set.
  if (template_render_file(context->gallery_config->templates_dir,
                           context->gallery_config->album_template, &template_context, context->os,
                           &perm, scratch, &page->html, log) != 0) {
    return -1;
  }
  arenas->perm = perm;
  return 0;
}
