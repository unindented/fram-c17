#include "build/page_renderer.h"

#include "build/job.h"
#include "build/template.h"
#include "core/error.h"
#include "domain/album.h"
#include "domain/gallery_config.h"

/**
 * Worker context shared by all album page render jobs. Every field is read-only to a job, except
 * `rendered_pages`, where each job writes its own slot.
 */
struct PageRenderContext {
  /** Configuration supplying the template directory and the album template name. */
  const struct GalleryConfig* gallery_config;

  /** Albums to render, one per job index. No album is written during the run. */
  const struct Album* const* albums;

  /** One rendered-page slot per album. A job writes only `rendered_pages[index]`. */
  struct RenderedPage* rendered_pages;
};

/**
 * @brief Renders one album page into its result slot.
 *
 * This is the `JobFn` of the page phase, so it runs concurrently with other indexes and writes only
 * `rendered_pages[index]` and its own error slot. A failure is latched through `job_set_error` with
 * the album named, and leaves the slot's `html` `NULL`.
 *
 * @param jobs     Running job set, whose error slot for `index` the job may fill through
 *                 `job_set_error`. Must not be `NULL`.
 * @param index    Album index this job handles. Must be below the album count.
 * @param userdata `struct PageRenderContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording the failure through `job_set_error`.
 */
static int render_album_page_job(struct JobSet* jobs, size_t index, void* userdata)
    __attribute__((nonnull(1, 3)));

int page_renderer_render_pages(const struct GalleryConfig* gallery_config,
                               const struct Album* const* albums,
                               size_t album_count,
                               size_t worker_count,
                               bool is_verbose,
                               struct RenderedPage* rendered_pages,
                               struct StringBuffer* error_out) {
  struct PageRenderContext context = {
      .gallery_config = gallery_config,
      .albums = albums,
      .rendered_pages = rendered_pages,
  };
  return job_run(album_count, worker_count, render_album_page_job, &context, "rendering albums",
                 is_verbose, error_out);
}

static int render_album_page_job(struct JobSet* jobs, size_t index, void* userdata) {
  const struct PageRenderContext* context = userdata;
  const struct Album* album = context->albums[index];
  const struct TemplateContext template_context = {
      .gallery_config = context->gallery_config,
      .album_current = album,
  };
  struct RenderedPage* page = &context->rendered_pages[index];
  char err[ERROR_MESSAGE_SIZE];
  page->html = template_render_file(context->gallery_config->templates_dir,
                                    context->gallery_config->album_template, &template_context,
                                    &page->html_len, err, sizeof(err));
  if (page->html == NULL) {
    // Parenthesize the attribution. The callee's message may itself end in a `: <reason>` clause. A
    // bare `for '%s'` suffix would read as part of that reason rather than as the album the render
    // was for.
    if (album_is_root(album)) {
      job_set_error(jobs, index, "%s (while rendering the root album)", err);
    } else {
      job_set_error(jobs, index, "%s (while rendering album '%s')", err, album->source_dir);
    }
    return -1;
  }
  return 0;
}
