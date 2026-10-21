#ifndef SHARED_ARENA_H
#define SHARED_ARENA_H

#include <stddef.h>

/**
 * Bump allocator over one region of memory. `beg` advances with each allocation, so the free space
 * is always `[beg, limit)`, and the part of it that is ready to use is `[beg, end)`.
 *
 * The arena owns no memory and frees nothing piecemeal. A region from `arena_reserve` starts as
 * address space only: `end` starts at `beg`, and an allocation past `end` makes more pages usable,
 * up to `limit`, before it returns. A region the caller already owns, such as a static array, is
 * usable from the start: it is `(struct Arena){.beg = beg, .end = end}`, with `limit` left `NULL`,
 * and an allocation past `end` does not fit. An allocation lives as long as its region, unless it
 * was made through a copy of the arena that is then dropped. `arena_discard` gives a buffer's pages
 * back early, which loses its bytes but leaves the buffer usable.
 *
 * Passing an arena by value hands the callee a copy of the bounds, so whatever the callee allocates
 * through its copy is reclaimed when it returns. Passing it by pointer makes the allocations
 * outlive the call, and so does writing a copy back over the original. A copy that makes pages
 * usable does not tell the original, which makes the same pages usable again the next time it needs
 * them. That costs a system call and changes nothing.
 *
 * In a build with the address sanitizer, an arena over a reserved region marks every byte it makes
 * usable as poisoned, unpoisons only the bytes an allocation returns, and leaves a poisoned gap
 * before each new allocation, so a read or write past the end of one allocation is reported.
 */
struct Arena {
  /** First free byte. */
  char* beg;

  /** One past the last byte that is ready to use. */
  char* end;

  /**
   * One past the last byte of the region, up to which an allocation may make pages usable, or
   * `NULL` when the region is `[beg, end)` and nothing more.
   */
  char* limit;
};

/**
 * @brief Reports that an arena ran out of space and ends the process. The application defines it.
 *
 * The library treats running out of memory as fatal rather than as an error return, so no
 * allocation can fail at its call site. A library cannot know its application's exit status or
 * diagnostic, so each application defines this once: a command-line tool writes a message and
 * exits, and a unit-test build exits with a status a test can tell apart from a crash. It must not
 * return.
 */
_Noreturn void os_oom(void);

/**
 * @brief Reserves `len` bytes of address space as an arena, without memory behind them.
 *
 * The region is private anonymous memory mapped with no access, so reserving it costs address space
 * but no memory, and a system with strict overcommit does not charge it. The arena's allocations
 * make pages readable and writable as they reach them, a step at a time, and only those pages count
 * against the system's memory.
 *
 * @param len       Bytes to reserve. Must be positive. Rounded up to a whole number of pages.
 * @param arena_out Receives an arena over the whole region, with nothing usable yet. Written only
 *                  on success. Must not be `NULL`.
 * @return `0` on success, or `-1` with `errno` set when the system refuses the address space, `len`
 *         exceeds `arena_reserve_len_max`, or `len` is within a page of `PTRDIFF_MAX`.
 */
int arena_reserve(ptrdiff_t len, struct Arena* arena_out) __attribute__((nonnull(2)));

/**
 * Cap on the `len` that `arena_reserve` takes, checked before the rounding to pages. A longer `len`
 * is refused with `ENOMEM`, as the system refuses a reservation under an address-space limit. A
 * program leaves it at `PTRDIFF_MAX`, and a test lowers it, while no other thread reserves, to
 * stand in for a limit such as `ulimit -v`.
 */
extern ptrdiff_t arena_reserve_len_max;

/** Most times `arena_reserve_halving` halves a size the system refuses before it gives up. */
enum { ARENA_RESERVE_HALVING_COUNT_MAX = 6 };

/**
 * @brief Reserves `len` bytes of address space as an arena, or a power-of-two fraction of it when
 *        the system refuses the full size.
 *
 * Under an address-space limit, such as `ulimit -v`, the system refuses a reservation larger than
 * what is left. Each refusal halves the size, up to `ARENA_RESERVE_HALVING_COUNT_MAX` times, and
 * the first size that fits is halved once more, so `malloc` and thread stacks keep room beside it.
 * A caller that splits the arena into regions divides each region's length by the same power of
 * two. Calls `os_oom` when even the smallest size is refused.
 *
 * @param len       Bytes to reserve at full size. Must be at least
 *                  `1 << (ARENA_RESERVE_HALVING_COUNT_MAX + 1)`, so the last halving still leaves a
 *                  positive size.
 * @param arena_out Receives an arena over the whole region, as from `arena_reserve`. Must not be
 *                  `NULL`.
 * @return The number of halvings: `0` when the full size fits, or otherwise from `2` to
 *         `ARENA_RESERVE_HALVING_COUNT_MAX + 1`.
 */
int arena_reserve_halving(ptrdiff_t len, struct Arena* arena_out) __attribute__((nonnull(2)));

/**
 * @brief Returns a region from `arena_reserve` or `arena_reserve_halving` to the system.
 *
 * Invalidates every allocation made from the region, and every arena split from it.
 *
 * @param region The arena that `arena_reserve` or `arena_reserve_halving` wrote to `arena_out`,
 *               before any allocation advanced it.
 */
void arena_release(struct Arena region);

/**
 * @brief Gives the memory behind a large buffer back to the system, keeping the buffer usable.
 *
 * Maps every whole page inside `[data, data + len)` anew, so it no longer counts as resident and
 * reads as zero until it is written again. This is where a `malloc` design would `free` the buffer,
 * for one the caller is done with but whose arena lives on. Does nothing over memory the caller
 * owns, whose `limit` is `NULL`, because the pages around it are not the arena's. Calls `os_oom`
 * when the system refuses the new pages.
 *
 * @param arena Arena the buffer was allocated from. Over a reserved region, the buffer must end at
 *              or before `arena->beg`. Must not be `NULL`.
 * @param data  First byte of the buffer. Its bytes are lost, so a buffer the caller holds as
 *              read-only may be passed.
 * @param len   Length of the buffer in bytes. Must not be negative.
 */
void arena_discard(const struct Arena* arena, const void* data, ptrdiff_t len)
    __attribute__((nonnull(1)));

/**
 * @brief Takes the first `len` bytes of the arena's free space as a region of its own.
 *
 * Neither zeroes nor makes usable any byte, so splitting a reserved region costs nothing until the
 * new arena allocates. The new arena's `limit` is the end of its share, so it never grows into the
 * bytes that follow. Over a reserved region, `len` is rounded down to a multiple of 16, so a region
 * that starts aligned leaves the next one aligned too, and two regions in use by two threads never
 * share bytes of the sanitizer's bookkeeping. Over a region the caller owns, the new arena is
 * `[beg, beg + len)` and nothing more, like its parent.
 *
 * @param arena Arena whose free space the region comes from. Must not be `NULL`.
 * @param len   Length of the region in bytes, before rounding. Must not be negative. Calls `os_oom`
 *              when the free space is shorter.
 * @return An arena over the region.
 */
struct Arena arena_split(struct Arena* arena, ptrdiff_t len) __attribute__((nonnull(1)));

/**
 * @brief Allocates zeroed storage for `count` objects of `size` bytes aligned to `align`.
 *
 * The bounds check cannot overflow: it compares `count` against the free space divided by `size`
 * rather than computing `count * size`. It is deliberately conservative. A request that would fill
 * the remaining space exactly is refused too, which keeps the check one comparison that also
 * refuses a request whose alignment padding alone exceeds the free space.
 *
 * The check compares against `end`, so an allocation that fits in the usable part costs one
 * comparison. One that does not makes more of the region usable first, and calls `os_oom` instead
 * of returning when the request does not fit before `limit`, so the result is never `NULL`.
 *
 * @param arena Arena to allocate from. Must not be `NULL`.
 * @param count Number of objects. Must not be negative. `0` returns a pointer to no storage.
 * @param size  Size of one object in bytes. Must be positive.
 * @param align Alignment in bytes. Must be a power of two.
 * @return Pointer to `count * size` zeroed bytes.
 */
void* arena_alloc(struct Arena* arena, ptrdiff_t count, ptrdiff_t size, ptrdiff_t align)
    __attribute__((nonnull(1), returns_nonnull));

/**
 * @brief Allocates `n` zeroed objects of type `t` from arena `a`.
 *
 * Derives the size and alignment from the type, so neither can disagree with the pointer the caller
 * stores. The name avoids `new`, which a tool parsing this header as C++ would reject.
 */
#define arena_new(a, n, t) ((t*)arena_alloc((a), (n), (ptrdiff_t)sizeof(t), (ptrdiff_t)_Alignof(t)))

/**
 * @brief Takes `count * size` bytes right at the start of the free space, without zeroing them.
 *
 * Adds no alignment padding and no gap, so the bytes continue the allocation that ends at `beg`.
 * That lets a growable array or buffer extend itself in place. Unlike `arena_alloc`, a request that
 * fills the free space exactly fits.
 *
 * @param arena Arena to take the bytes from. Must not be `NULL`.
 * @param count Number of objects. Must not be negative.
 * @param size  Size of one object in bytes. Must be positive.
 * @return The first of the bytes. Calls `os_oom` instead of returning when they do not fit.
 */
char* arena_extend(struct Arena* arena, ptrdiff_t count, ptrdiff_t size)
    __attribute__((nonnull(1), returns_nonnull));

/**
 * @brief Grows an array to make room for more elements, in place when it is the last allocation.
 *
 * Doubles `*capacity`, or sets it to 8 when it is 0. When `data` ends exactly where the arena's
 * free space begins, the arena extends it without copying. Otherwise the elements move to a fresh
 * allocation at the end of the arena and the old storage is abandoned. Because the capacity
 * doubles, the abandoned copies add up to less than the final array.
 *
 * @param arena    Arena that owns `data`. Must not be `NULL`.
 * @param data     Current elements, `*capacity` of them. May be `NULL` only when `*capacity` is 0.
 * @param capacity Current element capacity, updated to the new capacity. Must not be `NULL`.
 * @param size     Size of one element in bytes. Must be positive.
 * @param align    Alignment of one element in bytes. Must be a power of two.
 * @return The grown array, holding the old elements followed by zeroed new ones.
 */
void* arena_grow(struct Arena* arena,
                 void* data,
                 ptrdiff_t* capacity,
                 ptrdiff_t size,
                 ptrdiff_t align) __attribute__((nonnull(1, 3), returns_nonnull));

#endif
