#include "runtime/walk.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/ascii.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"

/** State that one directory walk carries from its root through every directory it reaches. */
struct Walk {
  /** Path list that receives the matching paths, unsorted until the walk ends. */
  struct PathList* paths;

  /** Host whose tree is walked. */
  struct Os* os;

  /**
   * Arena that owns `paths` and every path appended to it, and what lives as long as the walk: the
   * visited set and the deferred links, a few bytes per directory, which stay with the paths. Each
   * directory's entries and joined paths go in the caller's scratch instead, passed by value down
   * the recursion, so they are released as soon as the walk leaves that directory.
   */
  struct Arena* perm;

  /** Array of `suffix_count` suffixes to match. */
  const struct Str* suffixes;

  /** Number of entries in `suffixes`. Zero matches nothing. */
  ptrdiff_t suffix_count;

  /** Whether matching folds ASCII case. */
  bool is_fold_case;

  /** Operation leading every failure message. */
  struct Str operation;

  /** Log that receives a failure anywhere in the walk. */
  struct Log* log;

  /**
   * Scratch array of `visited_capacity` slots. The first `visited_count` hold the identity of every
   * directory walked so far and of the excluded directory, sorted by device and then inode, so a
   * directory reached a second time through a symlink, or the excluded one reached at all, is found
   * by binary search and skipped.
   */
  struct OsIdentity* visited;

  /** Number of populated entries in `visited`. */
  ptrdiff_t visited_count;

  /** Allocated slots in `visited`. */
  ptrdiff_t visited_capacity;

  /**
   * Symlinked directories found so far, in the order the walk found them. The walk reaches them
   * only after walking the root's own tree without following a symlink, so a directory with a path
   * of its own in that tree is listed under that path rather than under an alias to it.
   */
  struct PathList links;
};

/**
 * @brief Records `excluded_dir` as already walked, so every path that reaches it is skipped.
 *
 * @param walk         Walk state whose visited set is seeded. Must not be `NULL`.
 * @param excluded_dir Directory to leave out of the walk, or empty for none. A path with no
 *                     identity records nothing, because nothing below it can be listed.
 */
static void walk_files_with_suffixes_exclude(struct Walk* walk, struct Str excluded_dir)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether this is the walk's first visit to a directory, recording it when it is.
 *
 * @param walk     Walk state whose visited set is searched and extended. Must not be `NULL`.
 * @param identity Identity of the directory about to be walked or excluded. Must not be `NULL`.
 * @return `true` when `identity` had not been visited and is now recorded, or `false` when it had.
 */
static bool is_first_visit_recorded(struct Walk* walk, const struct OsIdentity* identity)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Walks one directory, appending matching files to `walk->paths` without sorting.
 *
 * Skips a directory the walk has already visited. Otherwise lists the directory's entries and
 * visits them in byte order, recursing into each real subdirectory and deferring each symlinked one
 * to `walk->links`. `os_list` closes the directory before this recurses, so one directory is open
 * at a time, whatever the depth of the tree.
 *
 * @param walk     Walk state. Must not be `NULL`.
 * @param dir_path Directory to walk. The tree root, a deferred symlink, or a real subdirectory
 *                 being scanned.
 * @param scratch  Arena for the directory's entries and the paths joined onto it, released when
 *                 this returns.
 * @return `0` on success, including a skipped directory, or `-1` on a directory failure, reported
 *         in `walk->log` naming the directory that failed.
 */
static int visit_dir(struct Walk* walk, struct Str dir_path, struct Arena scratch)
    __attribute__((nonnull(1)));

/**
 * @brief Reports that a directory or an entry could not be inspected.
 *
 * Kept out of line so its message buffer is not part of every level of a deep walk.
 *
 * @param walk         Walk state whose log receives the failure. Must not be `NULL`.
 * @param step         What could not be inspected: `directory` or `entry`.
 * @param path         Path of the directory or entry.
 * @param error_number `errno` value of the failed inspection.
 * @return `-1` always, so a caller can write `return report_inspect_failure(...);`.
 */
static int report_inspect_failure(const struct Walk* walk,
                                  struct Str step,
                                  struct Str path,
                                  int error_number) __attribute__((nonnull(1), noinline));

/**
 * @brief Compares two `struct OsEntry` items by name for `qsort`.
 *
 * @param a Pointer to the first entry. Must not be `NULL`.
 * @param b Pointer to the second entry. Must not be `NULL`.
 * @return A negative, zero, or positive value per `str_compare` of the two names.
 */
static int compare_entries(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Appends one matching regular file, descends into a real subdirectory, or defers a
 *        symlinked one.
 *
 * @param walk     Walk state. Must not be `NULL`.
 * @param dir_path Directory containing `entry`.
 * @param entry    Entry to visit. Must not be `NULL`.
 * @param scratch  Arena for the entry's joined path and the walk below a subdirectory, released
 *                 when this returns.
 * @return `0` on success (including skipped entries), or `-1` on failure, reported in `walk->log`.
 */
static int visit_dir_entry(struct Walk* walk,
                           struct Str dir_path,
                           const struct OsEntry* entry,
                           struct Arena scratch) __attribute__((nonnull(1, 3)));

/**
 * @brief Reports whether `text` ends with any of `suffixes`.
 *
 * @param text         String to test.
 * @param suffixes     Array of `suffix_count` suffixes. May be `NULL` only when `suffix_count` is
 *                     0.
 * @param suffix_count Number of entries in `suffixes`. Zero matches nothing.
 * @param is_fold_case Whether ASCII case differences are ignored, as in `has_suffix`.
 * @return `true` when `text` ends with at least one suffix, `false` otherwise.
 */
static bool has_any_suffix(struct Str text,
                           const struct Str* suffixes,
                           ptrdiff_t suffix_count,
                           bool is_fold_case);

/**
 * @brief Reports whether `text` ends with the literal `suffix`.
 *
 * @param text         String to test.
 * @param suffix       Suffix to look for.
 * @param is_fold_case Whether ASCII case differences are ignored, so `.JpG` matches `.jpg`. Bytes
 *                     outside ASCII always compare exactly.
 * @return `true` when `text` ends with `suffix`, `false` otherwise.
 */
static bool has_suffix(struct Str text, struct Str suffix, bool is_fold_case);

/**
 * @brief Compares two `PathList` items for `qsort`.
 *
 * @param a Pointer to the first `struct Str` element. Must not be `NULL`.
 * @param b Pointer to the second `struct Str` element. Must not be `NULL`.
 * @return A negative, zero, or positive value per `str_compare` of the two paths.
 */
static int compare_paths(const void* a, const void* b) __attribute__((nonnull(1, 2)));

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
                             struct Log* log) {
  struct Walk walk = {.paths = paths,
                      .os = os,
                      .perm = perm,
                      .suffixes = suffixes,
                      .suffix_count = suffix_count,
                      .is_fold_case = is_fold_case,
                      .operation = operation,
                      .log = log};
  walk_files_with_suffixes_exclude(&walk, excluded_dir);
  int rc = visit_dir(&walk, root_dir, scratch);
  // A deferred link can defer more links of its own, so this reads `count` on every iteration
  // rather than once.
  for (ptrdiff_t i = 0; rc == 0 && i < walk.links.count; i++) {
    rc = visit_dir(&walk, walk.links.items[i], scratch);
  }
  if (rc != 0) {
    return -1;
  }
  // Skip the empty case rather than let `qsort` see it. An empty list's `items` is `NULL`, and
  // passing a null pointer to `qsort` is undefined even with a count of 0.
  if (paths->count > 0) {
    qsort(paths->items, (size_t)paths->count, sizeof(*paths->items), compare_paths);
  }
  return 0;
}

static void walk_files_with_suffixes_exclude(struct Walk* walk, struct Str excluded_dir) {
  struct OsStat st;
  if (excluded_dir.len > 0 && os_stat(walk->os, excluded_dir, &st) == 0) {
    (void)is_first_visit_recorded(walk, &st.identity);
  }
}

static bool is_first_visit_recorded(struct Walk* walk, const struct OsIdentity* identity) {
  // Binary search for the first slot not below `identity`, which is where it is or belongs.
  ptrdiff_t low = 0;
  ptrdiff_t high = walk->visited_count;
  while (low < high) {
    const ptrdiff_t mid = low + (high - low) / 2;
    if (os_identity_compare(&walk->visited[mid], identity) < 0) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  if (low < walk->visited_count && os_identity_compare(&walk->visited[low], identity) == 0) {
    return false;
  }

  if (walk->visited_count == walk->visited_capacity) {
    walk->visited = arena_grow(walk->perm, walk->visited, &walk->visited_capacity,
                               sizeof(*walk->visited), _Alignof(struct OsIdentity));
  }
  // Inserting in place keeps the array sorted. The shift is linear, but the walk records each
  // directory once, and a content tree has far fewer directories than files.
  for (ptrdiff_t i = walk->visited_count; i > low; i--) {
    walk->visited[i] = walk->visited[i - 1];
  }
  walk->visited[low] = *identity;
  walk->visited_count++;
  return true;
}

static int visit_dir(struct Walk* walk, struct Str dir_path, struct Arena scratch) {
  // This inspection does not validate the type. `os_list` rejects a non-directory, which only the
  // root can be, because `visit_dir_entry` passes on only entries listed as directories.
  struct OsStat st;
  if (os_stat(walk->os, dir_path, &st) != 0) {
    return report_inspect_failure(walk, S("directory"), dir_path, errno);
  }
  if (!is_first_visit_recorded(walk, &st.identity)) {
    // This silently skips a directory the walk already reached by another path: a symlink back into
    // its own ancestry, or a second alias of one directory. Every file below it is already in
    // `paths` under the first path, so nothing is missing. Walking it again would list each file
    // once per path, and a chain of aliased directories multiplies that at every level. The
    // excluded directory is recorded before the walk starts, so it takes this skip on every path.
    return 0;
  }

  struct OsEntry* entries = NULL;
  ptrdiff_t count = 0;
  if (os_list(walk->os, dir_path, &scratch, &entries, &count, walk->operation, walk->log) != 0) {
    return -1;
  }
  // Listing order is whatever the host stores. Sorting makes the walk order, and with it which of
  // two aliases of one directory is walked, the same on every machine. An empty listing's array may
  // be `NULL`, which `qsort` must not see.
  if (count > 0) {
    qsort(entries, (size_t)count, sizeof(*entries), compare_entries);
  }
  int rc = 0;
  for (ptrdiff_t i = 0; rc == 0 && i < count; i++) {
    rc = visit_dir_entry(walk, dir_path, &entries[i], scratch);
  }
  return rc;
}

static int report_inspect_failure(const struct Walk* walk,
                                  struct Str step,
                                  struct Str path,
                                  int error_number) {
  char message[ERROR_SYSTEM_MESSAGE_SIZE];
  return log_error(walk->log, "%.*s: cannot inspect %.*s: %s ('%.*s')", STR_FMT(walk->operation),
                   STR_FMT(step), error_system_message(message, sizeof(message), error_number),
                   STR_FMT(path));
}

static int compare_entries(const void* a, const void* b) {
  return str_compare(((const struct OsEntry*)a)->name, ((const struct OsEntry*)b)->name);
}

static int visit_dir_entry(struct Walk* walk,
                           struct Str dir_path,
                           const struct OsEntry* entry,
                           struct Arena scratch) {
  // The joined path comes first in `scratch`, so the walk below a subdirectory, which gets the rest
  // of it, cannot reuse its bytes. A path kept past this entry is copied into the arena that keeps
  // it.
  const struct Str file_path = path_join(dir_path, entry->name, &scratch);
  switch (entry->kind) {
    case OS_ENTRY_DIR:
      return visit_dir(walk, file_path, scratch);
    case OS_ENTRY_DIR_LINK:
      // A symlinked directory waits until every directory reachable without a symlink is walked, so
      // the target's own path, when it has one in the tree, is the one that lists its files.
      path_list_push(&walk->links, file_path, walk->perm);
      return 0;
    case OS_ENTRY_FILE:
      if (has_any_suffix(entry->name, walk->suffixes, walk->suffix_count, walk->is_fold_case)) {
        path_list_push(walk->paths, file_path, walk->perm);
      }
      return 0;
    case OS_ENTRY_FAILED:
      // The listing kept the entry so that it fails here, in the sorted walk's order: after
      // everything below an earlier sibling, and before a later one.
      return report_inspect_failure(walk, S("entry"), file_path, entry->error_number);
  }
  return 0;
}

static bool has_any_suffix(struct Str text,
                           const struct Str* suffixes,
                           ptrdiff_t suffix_count,
                           bool is_fold_case) {
  for (ptrdiff_t i = 0; i < suffix_count; i++) {
    if (has_suffix(text, suffixes[i], is_fold_case)) {
      return true;
    }
  }
  return false;
}

static bool has_suffix(struct Str text, struct Str suffix, bool is_fold_case) {
  if (text.len < suffix.len) {
    return false;
  }
  const struct Str candidate = str_tail(text, text.len - suffix.len);
  for (ptrdiff_t i = 0; i < suffix.len; i++) {
    const unsigned char left = (unsigned char)candidate.data[i];
    const unsigned char right = (unsigned char)suffix.data[i];
    if (left != right && (!is_fold_case || ascii_to_lower(left) != ascii_to_lower(right))) {
      return false;
    }
  }
  return true;
}

static int compare_paths(const void* a, const void* b) {
  return str_compare(*(const struct Str*)a, *(const struct Str*)b);
}
