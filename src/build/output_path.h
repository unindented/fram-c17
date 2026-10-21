#ifndef FRAM_OUTPUT_PATH_H
#define FRAM_OUTPUT_PATH_H

#include <stddef.h>

#include "shared/str.h"

struct Arena;
struct Log;

/**
 * @brief Rejects a generated output path that exceeds either output-path limit.
 *
 * Every producer of an output path reports a limit failure through this one function, so the album,
 * media, aggregate, and static file paths share one wording. The limits themselves are applied by
 * `path_check_output_limits`.
 *
 * @param relative_path Output path relative to the output directory.
 * @param source_label  Producer the path was generated for, such as a media source or a config list
 *                      entry, named in the diagnostic.
 * @param log           Log that receives an error naming the limit that was exceeded. Must not be
 *                      `NULL`.
 * @return `0` when both limits hold, or `-1` when one is exceeded.
 */
int output_path_check_limits(struct Str relative_path, struct Str source_label, struct Log* log)
    __attribute__((nonnull(3)));

/**
 * @brief Returns the `../` prefix that leads from an output file's directory back to the output
 *        root.
 *
 * @param relative_path Output file path relative to the output directory, already accepted by
 *                      `output_path_check_limits`.
 * @param arena         Arena that owns the returned prefix. Must not be `NULL`.
 * @return One `../` per `/` in `relative_path`, or an empty string for a top-level file, owned by
 *         `arena`.
 */
struct Str output_path_to_root(struct Str relative_path, struct Arena* arena)
    __attribute__((nonnull(2)));

#endif
