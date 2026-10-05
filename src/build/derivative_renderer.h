#ifndef FRAM_DERIVATIVE_RENDERER_H
#define FRAM_DERIVATIVE_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

struct GalleryConfig;
struct MediaItem;
struct StringBuffer;

/**
 * @brief Generates stale derivatives and original copies in parallel.
 *
 * This is the second of the three parallel phases. When `media_count` is 0, it runs no job and
 * returns `0`. One job owns each media item. `video_frame_paths[index]` supplies the retained
 * auto-rotated JPEG for a video and may be `NULL` for an image. Every video frame is unlinked after
 * its job, including on failure. The caller retains and later frees the path string.
 *
 * Call this only after `manifest_builder_populate` accepts every output path. Jobs write
 * concurrently, and the manifest is what guarantees that no two of them target the same file.
 *
 * Each finished job reports progress when `is_verbose`. Failing jobs' diagnostics go to `error_out`
 * one per line, each distinct message once, up to `JOB_ERROR_REPORT_COUNT_MAX` of them plus a count
 * of the rest, so the caller reports them at a single boundary.
 *
 * @param gallery_config    Configured derivative sizes, crops, and qualities shared by every job.
 *                          Must not be `NULL`.
 * @param media             Probed media items, one job per item. Must not be `NULL`.
 * @param media_count       Number of items in `media`.
 * @param video_frame_paths Extracted video frame paths, one per item in `media`, `NULL` for an
 *                          image. Must not be `NULL`.
 * @param worker_count      Requested worker threads, as for `pool_run`.
 * @param is_verbose        Whether progress is printed to `stderr`.
 * @param error_out         Growable buffer that receives the collected derivative diagnostics. Must
 *                          not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a derivative job failed, the error slots could
 *         not be allocated, the worker pool could not start, or a diagnostic could not be appended.
 */
int derivative_renderer_generate(const struct GalleryConfig* gallery_config,
                                 struct MediaItem* const* media,
                                 size_t media_count,
                                 char* const* video_frame_paths,
                                 size_t worker_count,
                                 bool is_verbose,
                                 struct StringBuffer* error_out)
    __attribute__((nonnull(1, 2, 4, 7)));

#endif
