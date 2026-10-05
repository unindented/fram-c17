#ifndef FRAM_OUTPUT_PATH_H
#define FRAM_OUTPUT_PATH_H

#include <stddef.h>

struct Arena;

/**
 * @brief Rejects a generated output path that exceeds either output-path limit.
 *
 * Every producer of an output path reports a limit failure through this one function, so the album,
 * media, aggregate, and asset paths share one wording. The limits themselves are applied by
 * `path_check_output_limits`.
 *
 * @param relative_path Output path relative to the output directory. Must not be `NULL`.
 * @param label         Producer the path was generated for, such as a media source or a config key,
 *                      named in the diagnostic. Must not be `NULL`.
 * @param err           Destination for a diagnostic. May be `NULL` only when `err_len` is 0.
 * @param err_len       Size of `err` in bytes.
 * @return `0` when both limits hold, or `-1` naming the limit that was exceeded.
 */
int output_path_check_limits(const char* relative_path,
                             const char* label,
                             char* err,
                             size_t err_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the `../` prefix that leads from an output file's directory back to the output
 *        root.
 *
 * @param relative_path Output file path relative to the output directory, already accepted by
 *                      `output_path_check_limits`. Must not be `NULL`.
 * @param arena         Arena that owns the returned prefix. Must not be `NULL`.
 * @return One `../` per `/` in `relative_path`, or an empty string for a top-level file, owned by
 *         `arena`, or `NULL` on allocation failure.
 */
char* output_path_to_root(const char* relative_path, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

#endif
