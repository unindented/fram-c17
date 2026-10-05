#include "core/path_list.h"

#include <stdlib.h>

#include "core/grow.h"
#include "core/text.h"

/** First slot count allocated when an empty path list first grows. */
enum { PATH_LIST_CAPACITY_MIN = 16 };

void path_list_init(struct PathList* paths) {
  *paths = (struct PathList){0};
}

void path_list_free(struct PathList* paths) {
  for (size_t i = 0; i < paths->count; i++) {
    free(paths->items[i]);
  }
  free(paths->items);
  path_list_init(paths);
}

int path_list_push(struct PathList* paths, const char* file_path) {
  if (paths->count == paths->capacity) {
    size_t capacity_next = 0;
    size_t capacity_bytes_next = 0;
    if (grow_capacity(paths->capacity, PATH_LIST_CAPACITY_MIN, sizeof(*paths->items),
                      &capacity_next, &capacity_bytes_next) != 0) {
      return -1;
    }
    // Reallocating into a temporary keeps `paths->items` valid when `realloc` returns `NULL`,
    // because the old block is still allocated and still owned.
    char** items = realloc(paths->items, capacity_bytes_next);
    if (items == NULL) {
      return -1;
    }
    paths->capacity = capacity_next;
    paths->items = items;
  }
  // `count` advances only after the copy succeeds, so a failed `text_strdup` leaves the slot
  // uncounted. Two things depend on that: `path_list_free` frees exactly `count` slots, while the
  // slots from `count` to `capacity` hold whatever `realloc` left there. And every consumer reads
  // `items[i]` with no `NULL` check, starting with `compare_paths` in `runtime/fs.c`, which hands
  // it straight to `strcmp`.
  paths->items[paths->count] = text_strdup(file_path);
  if (paths->items[paths->count] == NULL) {
    return -1;
  }
  paths->count++;
  return 0;
}
