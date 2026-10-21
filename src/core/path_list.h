#ifndef FRAM_PATH_LIST_H
#define FRAM_PATH_LIST_H

#include <stddef.h>

#include "shared/str.h"

struct Arena;

/**
 * Growable list of path strings, all stored in one arena. A zeroed list is empty and ready to push
 * to.
 */
struct PathList {
  /**
   * Arena array of `capacity` slots. The first `count` hold arena-owned path strings, and the rest
   * are zeroed.
   */
  struct Str* items;

  /** Number of populated entries in `items`. */
  ptrdiff_t count;

  /** Allocated slots in `items`. */
  ptrdiff_t capacity;
};

/**
 * @brief Appends an arena-owned copy of `file_path` to the list.
 *
 * Grows the backing array as needed. Every push to one list must use the same arena, which owns the
 * array and every copy.
 *
 * @param paths     List to append to. Must not be `NULL`.
 * @param file_path Path to copy and store.
 * @param arena     Arena that owns the list's array and the copy. Must not be `NULL`.
 */
void path_list_push(struct PathList* paths, struct Str file_path, struct Arena* arena)
    __attribute__((nonnull(1, 3)));

#endif
