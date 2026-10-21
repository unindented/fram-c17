#include "core/path_list.h"

#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

void path_list_push(struct PathList* paths, struct Str file_path, struct Arena* arena) {
  if (paths->count == paths->capacity) {
    paths->items = arena_grow(arena, paths->items, &paths->capacity, sizeof(*paths->items),
                              _Alignof(struct Str));
  }
  paths->items[paths->count] = str_clone(file_path, arena);
  paths->count++;
}
