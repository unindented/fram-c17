#ifndef FRAM_GROW_H
#define FRAM_GROW_H

#include <stddef.h>

/**
 * @brief Computes the next capacity for a growable array and its size in bytes, refusing to
 *        overflow.
 *
 * This is the doubling policy for every array in this program that grows by one slot at a time, so
 * the overflow arithmetic lives in one place rather than being written again per container.
 * Doubling must not overflow `size_t`. The resulting slot count must not overflow when multiplied
 * by the element size.
 *
 * The byte total leaves through `capacity_bytes_out` because it is what the caller passes to
 * `realloc`. It is the value this function has just proved cannot overflow. Handing back only the
 * slot count leaves every caller to write `*capacity_out * elem_size` again. That is the exact
 * product the second guard exists for, safe only by an argument each caller then has to restate in
 * a comment.
 *
 * Both leave through out-parameters rather than as the return value on purpose. Returning one with
 * `0` for failure reads more naturally at one call site but would add a fourth failure-signaling
 * convention to a codebase that deliberately has exactly three: pointer/`NULL`, `int` `0`/`-1`, and
 * `bool`. Keeping the action shape costs one line per caller and keeps every failure check in the
 * program looking like every other one.
 *
 * @param capacity           Current capacity in slots.
 * @param capacity_min       Capacity to jump to from zero. `0` is rejected.
 * @param elem_size          Size of one slot in bytes. `0` is rejected. The returned capacity times
 *                           this cannot overflow.
 * @param capacity_out       Receives the next capacity on success. Must not be `NULL`.
 * @param capacity_bytes_out Receives that capacity in bytes on success, as
 *                           `*capacity_out * elem_size`. Must not be `NULL`.
 * @return `0` on success, or `-1` when `capacity_min` or `elem_size` is 0 or the next capacity
 *         would overflow.
 */
int grow_capacity(size_t capacity,
                  size_t capacity_min,
                  size_t elem_size,
                  size_t* capacity_out,
                  size_t* capacity_bytes_out) __attribute__((nonnull(4, 5)));

#endif
