#include "core/grow.h"

#include <stdint.h>

int grow_capacity(size_t capacity,
                  size_t capacity_min,
                  size_t elem_size,
                  size_t* capacity_out,
                  size_t* capacity_bytes_out) {
  if (capacity > SIZE_MAX / 2) {
    return -1;
  }
  const size_t next = capacity == 0 ? capacity_min : capacity * 2;
  // A zero minimum or a zero element size would succeed with a capacity nothing can be stored in,
  // and the caller's append would then run out of bounds. The `elem_size == 0` half also keeps the
  // division defined. It matches `arena_calloc`'s idiom.
  if (capacity_min == 0 || elem_size == 0 || next > SIZE_MAX / elem_size) {
    return -1;
  }
  *capacity_out = next;
  // This is computed here rather than left to the caller, because the line above just proved it
  // cannot overflow. A caller respelling it has no such proof at hand.
  *capacity_bytes_out = next * elem_size;
  return 0;
}
