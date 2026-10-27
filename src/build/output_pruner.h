#ifndef FRAM_OUTPUT_PRUNER_H
#define FRAM_OUTPUT_PRUNER_H

#include <stddef.h>

#include "shared/arena.h"

struct GalleryConfig;
struct Log;
struct Manifest;
struct Os;

/**
 * @brief Removes every entry below `output_dir` that the build did not write and does not pass
 *        through, so the output tree holds only what the current inputs produce.
 *
 * Run it once every output is written. An entry is kept when following it reaches a file the
 * manifest names or a directory between `output_dir` and one, compared by `(device, inode)` rather
 * than by name: a case-insensitive filesystem keeps the case an earlier build created, so a name
 * comparison would remove a current output whose name changed only in case. A symlink is removed as
 * a link and never descended, so what it points to is never touched, and removing a hard link
 * leaves the file under its other names. A directory that is `input_dir`, `templates_dir`, or
 * `static_dir`, such as a mount of one inside `output_dir`, is neither entered nor removed. A stale
 * directory is removed once everything inside it is.
 *
 * A failure to list a directory or remove an entry is reported, and the prune goes on with the rest
 * of the tree. A directory that a failure left non-empty is not reported again, because its removal
 * would fail for the same cause.
 *
 * @param manifest          Every output path of the build, each joined onto `output_dir`. Must not
 *                          be `NULL`.
 * @param gallery_config    Configuration supplying `output_dir` and the input roots. Must not be
 *                          `NULL`.
 * @param os                Host the output tree is on. Must not be `NULL`.
 * @param scratch           Arena for the kept identities and the listings.
 * @param removed_count_out Receives the number of entries removed, directories included. Must not
 *                          be `NULL`.
 * @param log               Log that receives every failure. Must not be `NULL`.
 * @return `0` when every stale entry was removed, or `-1` when one could not be listed or removed.
 */
int output_pruner_prune(const struct Manifest* manifest,
                        const struct GalleryConfig* gallery_config,
                        struct Os* os,
                        struct Arena scratch,
                        ptrdiff_t* removed_count_out,
                        struct Log* log) __attribute__((nonnull(1, 2, 3, 5, 6)));

#endif
