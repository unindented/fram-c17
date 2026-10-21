#ifndef FRAM_VIDEO_H
#define FRAM_VIDEO_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct Log;

/**
 * One video's temporary frame file. The probe phase creates it and the derivative phase removes it.
 * The path is allocated on the main thread before either phase, so a job only fills it in.
 */
struct VideoFrame {
  /**
   * Terminated `mkostemp` template in the temporary directory, which `video_extract_frame` replaces
   * in place with the name of the file it creates. `NULL` for a media item that is not a video.
   */
  char* path;

  /**
   * Whether `path` names a frame this build created and has not removed yet, so a cleanup unlinks
   * only real frames, never the unfilled template or a name another process reused. Only the job
   * that owns the frame writes it, and the main thread reads it after `pool_run` has joined the
   * workers, so it needs no atomics.
   */
  bool is_created;
};

/**
 * @brief Prepares a frame slot whose template names a file in the temporary directory.
 *
 * The directory is `TMPDIR`, or `/tmp` when that is unset or empty. Call this on the main thread,
 * because it reads the environment.
 *
 * @param frame Frame slot to prepare. Must not be `NULL`.
 * @param perm  Arena that owns the path, for as long as the frame may exist. Must not be `NULL`.
 */
void video_frame_init(struct VideoFrame* frame, struct Arena* perm) __attribute__((nonnull(1, 2)));

/**
 * @brief Removes a created frame, best effort.
 *
 * Does nothing unless `is_created` is set. It clears `is_created` only when the unlink succeeds or
 * finds no file, so a frame whose unlink failed for another reason stays marked and a later cleanup
 * tries again.
 *
 * @param frame Frame slot to clean up. Must not be `NULL`.
 */
void video_frame_remove(struct VideoFrame* frame) __attribute__((nonnull(1)));

/**
 * @brief Probes a video's duration through `ffprobe`, rounded down to milliseconds.
 *
 * @param file_path       Video to probe.
 * @param scratch         Arena for the process arguments.
 * @param duration_ms_out Receives the duration on success. Must not be `NULL`.
 * @param operation       Operation leading the failure message, which names `file_path`.
 * @param log             Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when `ffprobe` fails or prints no valid duration.
 */
int video_probe_duration(struct Str file_path,
                         struct Arena scratch,
                         ptrdiff_t* duration_ms_out,
                         struct Str operation,
                         struct Log* log) __attribute__((nonnull(3, 5)));

/**
 * @brief Extracts one auto-rotated JPEG frame through `ffmpeg` into the frame slot's file.
 *
 * The function creates the file with `mkostemp` on the slot's template, in place, and passes an
 * explicit JPEG muxer, so its suffix is irrelevant. It sets `is_created` as soon as the file
 * exists. On success the file holds the frame, and the caller removes it with `video_frame_remove`.
 * On failure the function has already removed it, best effort.
 *
 * @param frame       Frame slot from `video_frame_init`, not yet used. Must not be `NULL`.
 * @param file_path   Video to extract the frame from.
 * @param position_ms Position of the frame in milliseconds. Must not be negative.
 * @param scratch     Arena for the process arguments.
 * @param operation   Operation leading the failure message, which names `file_path`, or the frame
 *                    when the frame is what failed.
 * @param log         Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file cannot be created or `ffmpeg` fails or writes no
 *         frame.
 */
int video_extract_frame(struct VideoFrame* frame,
                        struct Str file_path,
                        ptrdiff_t position_ms,
                        struct Arena scratch,
                        struct Str operation,
                        struct Log* log) __attribute__((nonnull(1, 6)));

#endif
