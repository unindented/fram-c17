#ifndef FRAM_DERIVATIVE_RENDERER_H
#define FRAM_DERIVATIVE_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

#include "formats/image.h"
#include "shared/arena.h"

struct GalleryConfig;
struct JobArenas;
struct Log;
struct MediaItem;
struct Os;
struct OutputWriter;
struct VideoFrame;

/**
 * @brief Generates stale derivatives and original copies in parallel.
 *
 * This is the second of the three parallel phases. When `media_count` is 0, it runs no job and
 * returns `0`. One job owns each media item. `video_frames[index]` supplies the auto-rotated JPEG
 * the probe extracted for a video. A video the build cache answered for has no frame yet, and its
 * job extracts one only when a derivative is stale. Every video frame is removed after its job,
 * including on failure, best effort: a frame whose unlink fails stays marked for the caller's final
 * cleanup. A job reads its source into a copy of its worker's scratch and allocates its decoded
 * pixels with `malloc`, under a budget shared by every worker.
 *
 * Every output goes through `writer`: a fresh derivative or original copy is marked kept, and a
 * stale one written. A dry run marks each stale output written without making it, so it decodes and
 * encodes nothing.
 *
 * Call this only after `manifest_builder_populate` accepts every output path. Jobs write
 * concurrently, and the manifest is what guarantees that no two of them target the same file, so
 * each job marks only its own item's paths in it.
 *
 * Each finished job reports progress when `is_verbose`. Failing jobs' errors go to `log` in media
 * order, as `job_run` collects them.
 *
 * @param writer         Writer every output is written through, by every worker at once. Must not
 *                       be `NULL`.
 * @param gallery_config Configured derivative sizes, crops, and qualities shared by every job. Must
 *                       not be `NULL`.
 * @param media          Probed media items, one job per item. Must not be `NULL`.
 * @param media_count    Number of items in `media`.
 * @param video_frames   Frame slots, one per item in `media`, filled by the probe for a video it
 *                       probed, and by the job for one the build cache answered for. Must not be
 *                       `NULL`.
 * @param worker_count   Requested worker threads, as for `pool_run`.
 * @param worker_arenas  One `JobArenas` per requested worker, as for `job_run`. Must not be `NULL`.
 * @param is_verbose     Whether progress is printed to `stderr`.
 * @param os             Host the sources, frames and outputs are read from and progress printed on,
 *                       by every worker at once. Must not be `NULL`.
 * @param scratch        Arena for the phase's own bookkeeping, as for `job_run`.
 * @param log            Log that receives the collected derivative errors. Its arena must be none
 *                       of the others. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a derivative job failed or the worker pool
 *         could not start.
 */
int derivative_renderer_generate(const struct OutputWriter* writer,
                                 const struct GalleryConfig* gallery_config,
                                 struct MediaItem* const* media,
                                 ptrdiff_t media_count,
                                 struct VideoFrame* video_frames,
                                 ptrdiff_t worker_count,
                                 struct JobArenas* worker_arenas,
                                 bool is_verbose,
                                 struct Os* os,
                                 struct Arena scratch,
                                 struct Log* log) __attribute__((nonnull(1, 2, 3, 5, 7, 9, 11)));

/**
 * @brief Returns the scale a source is decoded at to write its stale derivatives.
 *
 * This is the smallest eighth at which every stale derivative's source region, cropped as
 * configured, is still at least twice the derivative in each dimension, so the resizer, and not the
 * decoder's DCT reduction, does the final reduction. A source too small for that at any reduced
 * scale decodes at full size.
 *
 * @param gallery_config Configured derivatives. Must not be `NULL`.
 * @param item           Media item, with one derivative per configured one. Must not be `NULL`.
 * @param is_stale       One flag per derivative of the item, `true` where the output is rewritten.
 *                       Must not be `NULL`.
 * @param width_px       Stored width of the source, as its JPEG header gives it.
 * @param height_px      Stored height of the source, as its JPEG header gives it.
 * @param orientation    Orientation the decoded source is given.
 * @return The decode scale, in eighths, from 1 through `IMAGE_SCALE_FULL`.
 */
ptrdiff_t derivative_renderer_decode_scale(const struct GalleryConfig* gallery_config,
                                           const struct MediaItem* item,
                                           const bool* is_stale,
                                           ptrdiff_t width_px,
                                           ptrdiff_t height_px,
                                           enum ImageOrientation orientation)
    __attribute__((nonnull(1, 2, 3)));

#endif
