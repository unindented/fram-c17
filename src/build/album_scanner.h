#ifndef FRAM_ALBUM_SCANNER_H
#define FRAM_ALBUM_SCANNER_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/arena.h"

struct Album;
struct GalleryConfig;
struct JobArenas;
struct Log;
struct MediaItem;
struct Os;
struct PathList;
struct VideoFrame;

/**
 * @brief Builds the complete album/media skeleton from sorted source paths.
 *
 * The root album is always present, including when `source_paths` is empty. Every album, media item
 * and string, and both returned pointer arrays, live in `perm`, so the skeleton is released with
 * that arena. Every source is planned even after one fails, so one scan reports every path-limit
 * failure. A failed scan writes no output and leaves `perm` as it found it.
 *
 * @param gallery_config  Gallery configuration. Must not be `NULL`.
 * @param source_paths    Lexicographically sorted, distinct full source paths below `input_dir`, as
 *                        the walk lists them. Must not be `NULL`.
 * @param perm            Arena that owns the skeleton. Must not be `NULL`.
 * @param scratch         Arena for the scan's own bookkeeping and the names and relative paths the
 *                        skeleton's paths are derived from, released when the scan returns.
 * @param albums_out      Receives the album pointer array, sorted by `source_dir` with the root at
 *                        index `0`. Must not be `NULL`.
 * @param album_count_out Receives its count. Must not be `NULL`.
 * @param media_out       Receives the flat media pointer array in source order. Must not be `NULL`.
 * @param media_count_out Receives its count. Must not be `NULL`.
 * @param log             Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` on invalid paths or output limits.
 */
int album_scanner_build_skeleton(const struct GalleryConfig* gallery_config,
                                 const struct PathList* source_paths,
                                 struct Arena* perm,
                                 struct Arena scratch,
                                 struct Album*** albums_out,
                                 ptrdiff_t* album_count_out,
                                 struct MediaItem*** media_out,
                                 ptrdiff_t* media_count_out,
                                 struct Log* log) __attribute__((nonnull(1, 2, 3, 5, 6, 7, 8, 9)));

/**
 * @brief Probes media in parallel and fills only its scalar probe fields.
 *
 * This is the first of the three parallel phases. When `media_count` is 0, it runs no job and
 * returns `0`. `video_frames` holds one slot per item, prepared with `video_frame_init` for a
 * video. Each video job creates its slot's frame file. On success every video slot names a frame
 * that still exists. On failure, successful video jobs may still have created theirs. In both cases
 * the caller must eventually consume them or call `album_scanner_remove_video_frames`. A job reads
 * its source into a copy of its worker's scratch and allocates nothing that outlives it.
 *
 * Each finished job reports progress when `is_verbose`. Failing jobs' errors go to `log` in media
 * order, as `job_run` collects them.
 *
 * @param gallery_config Gallery configuration. Must not be `NULL`.
 * @param media          Flat media pointer array. Must not be `NULL`.
 * @param media_count    Number of items and frame slots.
 * @param video_frames   Caller-owned frame slots, one per item in `media`. Must not be `NULL`.
 * @param worker_count   Requested worker threads, as for `pool_run`.
 * @param worker_arenas  One `JobArenas` per requested worker, as for `job_run`. Must not be `NULL`.
 * @param is_verbose     Whether progress is printed to `stderr`.
 * @param os             Host the sources and frames are read from and progress printed on, by every
 *                       worker at once. Must not be `NULL`.
 * @param scratch        Arena for the phase's own bookkeeping, as for `job_run`.
 * @param log            Log that receives the collected probe errors. Its arena must be none of the
 *                       others. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a probe job failed or the worker pool could
 *         not start.
 */
int album_scanner_probe_media(const struct GalleryConfig* gallery_config,
                              struct MediaItem* const* media,
                              ptrdiff_t media_count,
                              struct VideoFrame* video_frames,
                              ptrdiff_t worker_count,
                              struct JobArenas* worker_arenas,
                              bool is_verbose,
                              struct Os* os,
                              struct Arena scratch,
                              struct Log* log) __attribute__((nonnull(1, 2, 4, 6, 8, 10)));

/**
 * @brief Removes every frame a slot still marks as created, best effort.
 *
 * A slot whose frame was never created, or was already removed, is left alone, so the cleanup never
 * unlinks an unfilled template or a name another process reused. A frame whose unlink fails stays
 * marked.
 *
 * @param video_frames Frame slots. May be `NULL` only when `media_count` is zero.
 * @param media_count  Number of slots.
 */
void album_scanner_remove_video_frames(struct VideoFrame* video_frames, ptrdiff_t media_count);

#endif
