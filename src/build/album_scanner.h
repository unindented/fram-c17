#ifndef FRAM_ALBUM_SCANNER_H
#define FRAM_ALBUM_SCANNER_H

#include <stdbool.h>
#include <stddef.h>

struct Album;
struct GalleryConfig;
struct MediaItem;
struct PathList;
struct StringBuffer;

/**
 * @brief Builds the complete album/media skeleton from sorted source paths.
 *
 * The root album is always present, including when `source_paths` is empty. Each returned album is
 * separately heap allocated and owns its strings, media records, and child/direct-media arrays in
 * its arena. The caller owns both returned pointer arrays. Release the result with
 * `album_scanner_free_skeleton`.
 *
 * @param gallery_config  Gallery configuration. Must not be `NULL`.
 * @param source_paths    Lexicographically sorted full source paths. Must not be `NULL`.
 * @param albums_out      Receives the malloc-owned album pointer array. Must not be `NULL`.
 * @param album_count_out Receives its count. Must not be `NULL`.
 * @param media_out       Receives the malloc-owned flat media pointer array in source order. Must
 *                        not be `NULL`.
 * @param media_count_out Receives its count. Must not be `NULL`.
 * @param err             Destination for a diagnostic. May be `NULL` only when `err_len` is 0.
 * @param err_len         Size of `err` in bytes.
 * @return `0` on success, or `-1` on invalid paths, output limits, or allocation failure.
 */
int album_scanner_build_skeleton(const struct GalleryConfig* gallery_config,
                                 const struct PathList* source_paths,
                                 struct Album*** albums_out,
                                 size_t* album_count_out,
                                 struct MediaItem*** media_out,
                                 size_t* media_count_out,
                                 char* err,
                                 size_t err_len) __attribute__((nonnull(1, 2, 3, 4, 5, 6)));

/**
 * @brief Releases a skeleton returned by `album_scanner_build_skeleton`.
 *
 * @param albums      Album pointer array, or `NULL` when `album_count` is zero.
 * @param album_count Number of album pointers.
 * @param media       Flat media pointer array, which may be `NULL`.
 */
void album_scanner_free_skeleton(struct Album** albums,
                                 size_t album_count,
                                 struct MediaItem** media);

/**
 * @brief Probes media in parallel and fills only its scalar probe fields.
 *
 * This is the first of the three parallel phases. When `media_count` is 0, it runs no job and
 * returns `0`. `video_frame_paths` must point to `media_count` initially-`NULL` slots. On success,
 * each video slot owns one malloc-owned temporary JPEG path that still exists, and image slots
 * remain `NULL`. On failure, successful video jobs may still have published paths. In both cases
 * the caller must eventually consume them or call `album_scanner_free_video_frames`.
 *
 * Each finished job reports progress when `is_verbose`. Failing jobs' diagnostics go to `error_out`
 * one per line, each distinct message once, up to `JOB_ERROR_REPORT_COUNT_MAX` of them plus a count
 * of the rest, so the caller reports them at a single boundary.
 *
 * @param gallery_config    Gallery configuration. Must not be `NULL`.
 * @param media             Flat media pointer array. Must not be `NULL`.
 * @param media_count       Number of items and frame-path slots.
 * @param video_frame_paths Caller-owned parallel frame-path array. Must not be `NULL`.
 * @param worker_count      Requested worker threads, as for `pool_run`.
 * @param is_verbose        Whether progress is printed to `stderr`.
 * @param error_out         Growable buffer that receives the collected probe diagnostics. Must not
 *                          be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a probe job failed, the error slots could not
 *         be allocated, the worker pool could not start, or a diagnostic could not be appended.
 */
int album_scanner_probe_media(const struct GalleryConfig* gallery_config,
                              struct MediaItem* const* media,
                              size_t media_count,
                              char** video_frame_paths,
                              size_t worker_count,
                              bool is_verbose,
                              struct StringBuffer* error_out) __attribute__((nonnull(1, 2, 4, 7)));

/**
 * @brief Unlinks and frees every retained video frame path, then clears its slot.
 *
 * @param video_frame_paths Parallel path array. May be `NULL` when `media_count` is zero.
 * @param media_count       Number of slots.
 */
void album_scanner_free_video_frames(char** video_frame_paths, size_t media_count);

#endif
