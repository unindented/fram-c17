#ifndef FRAM_PATH_LIST_H
#define FRAM_PATH_LIST_H

#include <stddef.h>

/** Growable list of path strings. */
struct PathList {
  /**
   * Heap array of `capacity` slots. The first `count` hold path strings owned by the list, and the
   * rest are uninitialized.
   */
  char** items;

  /** Number of populated entries in `items`. */
  size_t count;

  /** Allocated slots in `items`. */
  size_t capacity;
};

/**
 * @brief Initializes an empty path list with no heap allocation.
 *
 * @param paths List handle to prepare. Must not be `NULL`.
 */
void path_list_init(struct PathList* paths) __attribute__((nonnull(1)));

/**
 * @brief Releases all path strings and resets the list for reuse.
 *
 * Leaves the list initialized, so it may be reused without `path_list_init`.
 *
 * @param paths List to release. Must not be `NULL`.
 */
void path_list_free(struct PathList* paths) __attribute__((nonnull(1)));

/**
 * @brief Appends a heap-owned copy of `file_path` to the list.
 *
 * Grows the backing array as needed. The list owns the copy and frees it in `path_list_free`.
 *
 * @param paths     List to append to. Must not be `NULL`.
 * @param file_path Terminated path to copy and store. Must not be `NULL`.
 * @return `0` on success, or `-1` on size overflow or allocation failure.
 */
int path_list_push(struct PathList* paths, const char* file_path) __attribute__((nonnull(1, 2)));

#endif
