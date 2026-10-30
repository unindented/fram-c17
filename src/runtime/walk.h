#ifndef FRAM_WALK_H
#define FRAM_WALK_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct Log;
struct Os;
struct PathList;

/**
 * @brief Recursively lists regular files under `root_dir` whose name ends with any of `suffixes`.
 *
 * Appends each matching file's path to `paths` and sorts the whole list so builds are reproducible.
 * A match may fold ASCII case, which lets `.JpG` match `.jpg` without making behavior
 * locale-dependent. An empty suffix matches every regular file, and a zero suffix count matches
 * none.
 *
 * It follows symlinked directories but walks each directory once, identified by device and inode,
 * so a file below a directory reachable by several paths is listed once, not once per path. A
 * symlink back into its own ancestry and a second alias of a directory are both skipped silently,
 * because every file below them is already listed. The listed path is the one reached without
 * following a symlink when the tree has one. Otherwise it is the first path through a symlink, in a
 * walk that visits entries in byte order and each symlinked directory only after every real one. An
 * entry that resolves to nothing is skipped and one that cannot be inspected fails the walk, as
 * `os_list` describes, so a file this function could not look at is never silently missing from
 * `paths`.
 *
 * It skips `excluded_dir` and everything below it, compared by identity rather than by path text,
 * so the skip holds on every path that reaches it: its own path in the tree, a symlinked alias, or
 * `root_dir` itself. A build passes its `output_dir` here for every input tree it walks, so a file
 * that an earlier build generated is never read back as an input, even through a symlink into the
 * output tree. An `excluded_dir` that does not exist yet holds no file and excludes nothing.
 *
 * Only the operations in `runtime/os.h` touch the host, so the walk behaves the same on every
 * implementation of them.
 *
 * @param paths        Path list that receives the matching paths. Must not be `NULL`.
 * @param root_dir     Directory tree to walk.
 * @param excluded_dir Directory left out of the walk, or empty to walk the whole tree. A path with
 *                     no identity excludes nothing.
 * @param suffixes     Array of `suffix_count` suffixes, such as `.jpg`. May be `NULL` only when
 *                     `suffix_count` is 0.
 * @param suffix_count Number of suffixes. Zero matches no files.
 * @param is_fold_case Whether ASCII case differences are ignored when matching a suffix.
 * @param os           Host whose tree is walked. Must not be `NULL`.
 * @param perm         Arena that owns the paths appended to `paths`, the list's array, and the
 *                     walk's own bookkeeping, a few bytes per directory. Must not be `NULL`.
 * @param scratch      Arena for the listings of the directories the walk is inside, released when
 *                     the walk returns.
 * @param operation    Operation leading the failure message, such as `failed to list static files`.
 *                     The message names the exact path the failure happened on: the directory that
 *                     could not be inspected, opened, read or closed, or the entry that could not
 *                     be inspected. A caller must not add the root it passed, because the message
 *                     is already more precise than that.
 * @param log          Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a directory or entry failure.
 */
int walk_files_with_suffixes(struct PathList* paths,
                             struct Str root_dir,
                             struct Str excluded_dir,
                             const struct Str* suffixes,
                             ptrdiff_t suffix_count,
                             bool is_fold_case,
                             struct Os* os,
                             struct Arena* perm,
                             struct Arena scratch,
                             struct Str operation,
                             struct Log* log) __attribute__((nonnull(1, 7, 8, 11)));

#endif
