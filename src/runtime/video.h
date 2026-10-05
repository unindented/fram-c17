#ifndef FRAM_VIDEO_H
#define FRAM_VIDEO_H

#include <stddef.h>

/** Probes a video's duration through `ffprobe`, rounded down to milliseconds. */
int video_probe_duration(const char* file_path,
                         size_t* duration_ms_out,
                         char* reason,
                         size_t reason_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Extracts one auto-rotated JPEG frame through `ffmpeg`.
 *
 * The function creates a secure temporary with `mkstemp` and passes an explicit JPEG muxer, so its
 * suffix is irrelevant. On success the caller owns `*jpeg_path_out` and must unlink and free it.
 */
int video_extract_frame(const char* file_path,
                        size_t position_ms,
                        char** jpeg_path_out,
                        char* reason,
                        size_t reason_len) __attribute__((nonnull(1, 3)));

#endif
