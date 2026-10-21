// `MAP_ANON` is outside strict POSIX, so a `-std=c17` compile hides it unless the platform's
// feature-test macro asks for it: `_DARWIN_C_SOURCE` on macOS, `_DEFAULT_SOURCE` on glibc and musl.
// Both must precede every `#include`.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "shared/arena.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "asan.h"

/** Capacity `arena_grow` gives an array that has none yet. */
enum { ARENA_GROW_CAPACITY_MIN = 8 };

/**
 * Multiple that `arena_split` rounds a reserved region's length down to. It is a multiple of the
 * sanitizer's 8-byte granule, and of the alignment of every standard type.
 */
enum { ARENA_SPLIT_ALIGN = 16 };

/**
 * Bytes past an allocation's end that the slow path makes usable as well, so a run of small
 * allocations costs one system call for each mebibyte rather than one for each page. A larger step
 * would make fewer calls, but under strict overcommit each region would be charged for more memory
 * than it uses.
 */
enum { ARENA_COMMIT_STEP_LEN = 1024 * 1024 };

/**
 * Poisoned bytes `arena_alloc` leaves before each allocation under the address sanitizer, and the
 * least alignment it then gives the allocation. The sanitizer tracks memory in 8-byte granules and
 * can mark only a prefix of a granule addressable, so an allocation that started inside a granule
 * would make the bytes before it in that granule addressable too. Starting each allocation on a
 * granule keeps the whole gap poisoned.
 */
enum { ARENA_GAP_LEN = SHARED_ASAN ? 16 : 0, ARENA_GAP_ALIGN = 8 };

/**
 * @brief Makes the arena usable up to `target`, and a step past it where the region allows.
 *
 * Makes the pages from `end` up to the new end readable and writable. A page that a copy of the
 * arena already made usable is made usable again, which changes nothing. Under the address
 * sanitizer it poisons the arena's whole free space, so bytes a dead copy unpoisoned are poisoned
 * again before they are handed out.
 *
 * @param arena  Arena over a reserved region. Must not be `NULL`.
 * @param target First byte past the bytes the caller is about to take. At most `arena->limit`.
 */
static void arena_commit(struct Arena* arena, char* target) __attribute__((nonnull(1, 2)));

/**
 * @brief Unpoisons the bytes an allocation returns and poisons the gap after them.
 *
 * Does nothing without the address sanitizer, and nothing over memory the caller owns, whose poison
 * the arena did not set and must not change.
 *
 * @param arena Arena the bytes came from, already advanced past them. Must not be `NULL`.
 * @param data  First byte of the allocation.
 * @param len   Length of the allocation in bytes.
 */
static void arena_mark_allocated(const struct Arena* arena, char* data, ptrdiff_t len)
    __attribute__((nonnull(1)));

/**
 * @brief Returns the system's page size in bytes.
 *
 * @return The page size, a power of two.
 */
static ptrdiff_t arena_page_len(void);

ptrdiff_t arena_reserve_len_max = PTRDIFF_MAX;

int arena_reserve(ptrdiff_t len, struct Arena* arena_out) {
  assert(len > 0);
  const ptrdiff_t page_len = arena_page_len();
  if (len > arena_reserve_len_max || len > PTRDIFF_MAX - page_len) {
    errno = ENOMEM;
    return -1;
  }
  len = (len + page_len - 1) / page_len * page_len;
  void* region = mmap(NULL, (size_t)len, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (region == MAP_FAILED) {
    return -1;
  }
  *arena_out = (struct Arena){region, region, (char*)region + len};
  return 0;
}

int arena_reserve_halving(ptrdiff_t len, struct Arena* arena_out) {
  assert(len >> (ARENA_RESERVE_HALVING_COUNT_MAX + 1) > 0);
  int shift = 0;
  while (arena_reserve(len >> shift, arena_out) != 0) {
    if (++shift > ARENA_RESERVE_HALVING_COUNT_MAX) {
      os_oom();
    }
  }
  if (shift > 0) {
    // The size that fits is halved once more, so the rest of the process keeps as much room.
    arena_release(*arena_out);
    if (arena_reserve(len >> ++shift, arena_out) != 0) {
      os_oom();
    }
  }
  return shift;
}

void arena_release(struct Arena region) {
  assert(region.limit != NULL);
  // The sanitizer's `munmap` interceptor clears the shadow of the range, so a later mapping at the
  // same addresses inherits no poison.
  (void)munmap(region.beg, (size_t)(region.limit - region.beg));
}

void arena_discard(const struct Arena* arena, const void* data, ptrdiff_t len) {
  assert(len >= 0);
  if (arena->limit == NULL) {
    return;
  }
  // A buffer allocated from this arena ends at or before `arena->beg`. One that ends past it was
  // passed with the wrong arena or too long a `len`, and in the second case the remap can reach
  // past the buffer into the free space, or into a neighboring region split from the same
  // reservation.
  assert((uintptr_t)data + (uintptr_t)len <= (uintptr_t)arena->beg);
  const uintptr_t page_mask = (uintptr_t)arena_page_len() - 1;
  const uintptr_t first = ((uintptr_t)data + page_mask) & ~page_mask;
  const uintptr_t last = ((uintptr_t)data + (uintptr_t)len) & ~page_mask;
  // A fresh private mapping over the same pages drops their contents, so they stop counting as
  // resident until they are written again; `madvise` leaves them resident on macOS. The new pages
  // are readable and writable, as the arena had made them, so the bytes stay usable. Under strict
  // overcommit the system may refuse to charge them again, which is running out of memory.
  if (last > first && mmap((void*)first, (size_t)(last - first), PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
    os_oom();
  }
}

struct Arena arena_split(struct Arena* arena, ptrdiff_t len) {
  assert(len >= 0);
  if (arena->limit == NULL) {
    if (len > arena->end - arena->beg) {
      os_oom();
    }
    const struct Arena region = {arena->beg, arena->beg + len, NULL};
    arena->beg += len;
    return region;
  }
  // Two threads may use neighboring regions at once, and the sanitizer's poison of one 8-byte
  // granule covers bytes on both sides of a boundary inside it. Ending each region on a multiple of
  // `ARENA_SPLIT_ALIGN` keeps every granule in one region. Rounding before the check lets a caller
  // split a block into the shares it was sized from, each rounded down.
  len -= len % ARENA_SPLIT_ALIGN;
  if (len > arena->limit - arena->beg) {
    os_oom();
  }
  struct Arena region = {arena->beg, arena->end, arena->beg + len};
  if (region.end > region.limit) {
    region.end = region.limit;
  }
  arena->beg += len;
  // The usable part may now be empty. `end` never falls behind `beg`, so the fast path never sees
  // negative free space.
  if (arena->end < arena->beg) {
    arena->end = arena->beg;
  }
  return region;
}

void* arena_alloc(struct Arena* arena, ptrdiff_t count, ptrdiff_t size, ptrdiff_t align) {
  assert(count >= 0);
  assert(size > 0);
  assert(align > 0 && (align & (align - 1)) == 0);
  // Over a reserved region a sanitizer build skips a gap first and starts on a granule. Everywhere
  // else the gap is zero, and the compiler drops both.
  const ptrdiff_t gap = arena->limit != NULL ? ARENA_GAP_LEN : 0;
  if (gap > 0 && align < ARENA_GAP_ALIGN) {
    align = ARENA_GAP_ALIGN;
  }
  // The padding is the gap plus the distance from there up to the next multiple of `align`.
  // Negating the address as an unsigned value and masking gives that distance without a branch.
  const ptrdiff_t pad =
      gap + (ptrdiff_t)(-((uintptr_t)arena->beg + (uintptr_t)gap) & (uintptr_t)(align - 1));
  // Dividing the free space by `size` rather than multiplying `count` by it keeps the check from
  // overflowing for any `count`. When the padding exceeds the free space the quotient is zero or
  // negative, and the `>=` refuses that too.
  if (count >= (arena->end - arena->beg - pad) / size) {
    if (arena->limit == NULL || count >= (arena->limit - arena->beg - pad) / size) {
      os_oom();
    }
    arena_commit(arena, arena->beg + pad + count * size);
  }
  char* result = arena->beg + pad;
  arena->beg += pad + count * size;
  arena_mark_allocated(arena, result, count * size);
  return memset(result, 0, (size_t)(count * size));
}

char* arena_extend(struct Arena* arena, ptrdiff_t count, ptrdiff_t size) {
  assert(count >= 0);
  assert(size > 0);
  // `count * size` fits in the free space exactly when `count` is at most the quotient, so this
  // accepts an exact fit, and the division keeps it from overflowing.
  if (count > (arena->end - arena->beg) / size) {
    if (arena->limit == NULL || count > (arena->limit - arena->beg) / size) {
      os_oom();
    }
    arena_commit(arena, arena->beg + count * size);
  }
  char* result = arena->beg;
  arena->beg += count * size;
  arena_mark_allocated(arena, result, count * size);
  return result;
}

void* arena_grow(struct Arena* arena,
                 void* data,
                 ptrdiff_t* capacity,
                 ptrdiff_t size,
                 ptrdiff_t align) {
  // An element size that is not a multiple of its alignment would put padding between the old
  // elements and the extension, so the in-place path below would leave a gap inside the array. No C
  // type has that shape, because `sizeof` is always a multiple of `_Alignof`.
  assert(size % align == 0);
  const ptrdiff_t capacity_old = *capacity;
  if (data == NULL || (char*)data + (capacity_old * size) != arena->beg) {
    void* copy = arena_alloc(arena, capacity_old, size, align);
    // `memcpy` requires valid pointers even for zero bytes, and `data` may be `NULL` here.
    if (data != NULL) {
      memcpy(copy, data, (size_t)(capacity_old * size));
    }
    data = copy;
  }
  // `data` now ends where the free space begins, so this extends it in place.
  const ptrdiff_t extend = capacity_old > 0 ? capacity_old : ARENA_GROW_CAPACITY_MIN;
  (void)memset(arena_extend(arena, extend, size), 0, (size_t)(extend * size));
  *capacity = capacity_old + extend;
  return data;
}

static void arena_commit(struct Arena* arena, char* target) {
  char* end =
      arena->limit - target > ARENA_COMMIT_STEP_LEN ? target + ARENA_COMMIT_STEP_LEN : arena->limit;
  // `mprotect` takes whole pages. Rounding `end` down repeats pages already usable. Rounding the
  // new end up can reach into the next region when this one was split from a larger reservation,
  // which makes that page usable early and changes nothing else. The reservation itself ends on a
  // page boundary, so the range never leaves it.
  const ptrdiff_t page_len = arena_page_len();
  char* from = arena->end - (ptrdiff_t)((uintptr_t)arena->end & (uintptr_t)(page_len - 1));
  const ptrdiff_t tail = (ptrdiff_t)((uintptr_t)end & (uintptr_t)(page_len - 1));
  char* to = tail == 0 ? end : end + (page_len - tail);
  // A strict overcommit policy refuses the memory here, which is running out of it like any other.
  if (mprotect(from, (size_t)(to - from), PROT_READ | PROT_WRITE) != 0) {
    os_oom();
  }
#if SHARED_ASAN
  ASAN_POISON_MEMORY_REGION(arena->beg, (size_t)(end - arena->beg));
#endif
  arena->end = end;
}

static void arena_mark_allocated(const struct Arena* arena, char* data, ptrdiff_t len) {
#if SHARED_ASAN
  if (arena->limit != NULL) {
    ASAN_UNPOISON_MEMORY_REGION(data, (size_t)len);
    const ptrdiff_t usable = arena->end - arena->beg;
    ASAN_POISON_MEMORY_REGION(arena->beg,
                              (size_t)(usable < ARENA_GAP_LEN ? usable : ARENA_GAP_LEN));
  }
#else
  (void)arena;
  (void)data;
  (void)len;
#endif
}

static ptrdiff_t arena_page_len(void) {
  return (ptrdiff_t)sysconf(_SC_PAGESIZE);
}
