#include "shared/arena.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Minimum chunk payload in bytes, and the default for normal-sized allocations. This size amortizes
 * one `malloc` across many small allocations and bounds the waste in a partly filled chunk. A
 * request at least this large gets its own chunk sized exactly to fit it.
 */
enum { ARENA_CHUNK_CAPACITY_MIN = 8192 };

/** Heap chunk of bump-allocated storage owned by an arena. */
struct ArenaChunk {
  /**
   * Next chunk in the arena's ownership list. The list has no ordering requirement, because only
   * `arena_free` walks it. That is why `arena_alloc_chunk` may splice a full chunk in behind the
   * head.
   */
  struct ArenaChunk* next;

  /** Bytes of `data` already handed out. */
  size_t used;

  /** Total usable bytes in `data`. */
  size_t capacity;

  /** Payload region: `capacity` bytes, `max_align_t`-aligned so any type fits. */
  max_align_t data[];
};

/**
 * @brief Allocates a fresh chunk for an aligned request that the head cannot satisfy.
 *
 * An oversized request gets a chunk sized to fit it. When a head already exists, the full chunk is
 * linked behind the still-current head. Otherwise it becomes the new head. A normal request gets a
 * default-sized chunk that becomes the new head.
 *
 * @param arena        Arena the new chunk is linked into. Must not be `NULL`.
 * @param head         Current head chunk, or `NULL` when the arena is empty.
 * @param size_aligned Requested size, already rounded up to `max_align_t`.
 * @return Pointer to the request's storage within the new chunk, or `NULL` on allocation failure.
 */
static void* arena_alloc_chunk(struct Arena* arena, struct ArenaChunk* head, size_t size_aligned)
    __attribute__((nonnull(1)));

void arena_init(struct Arena* arena) {
  arena->head = NULL;
}

void arena_free(struct Arena* arena) {
  struct ArenaChunk* chunk = arena->head;
  while (chunk != NULL) {
    struct ArenaChunk* next = chunk->next;
    free(chunk);
    chunk = next;
  }
  arena_init(arena);
}

void* arena_alloc(struct Arena* arena, size_t size) {
  // A zero-byte request still needs a distinct, usable allocation.
  if (size == 0) {
    size = 1;
  }
  // Round up to `max_align_t` so bumping keeps every returned slot aligned.
  const size_t alignment = alignof(max_align_t);
  // Check before rounding up. For a `size_t` near the top of the range, `size + (alignment - 1)`
  // wraps. That would hand back a slot far smaller than requested, and the caller would overrun
  // it.
  if (size > SIZE_MAX - (alignment - 1)) {
    return NULL;
  }
  const size_t size_aligned = (size + (alignment - 1)) / alignment * alignment;

  struct ArenaChunk* head = arena->head;
  // Compare against the bytes remaining rather than `used + size_aligned` against `capacity`, so a
  // huge request cannot wrap the sum and appear to fit. The subtraction cannot underflow because
  // every path keeps `used <= capacity`.
  if (head == NULL || size_aligned > head->capacity - head->used) {
    return arena_alloc_chunk(arena, head, size_aligned);
  }
  void* ptr = (char*)head->data + head->used;
  head->used += size_aligned;
  return ptr;
}

void* arena_calloc(struct Arena* arena, size_t count, size_t size) {
  // The `size != 0` half guards the division below, not the product. Division by zero is undefined
  // behavior. A zero `size` needs no product check, because `count * 0` cannot overflow.
  if (size != 0 && count > SIZE_MAX / size) {
    return NULL;
  }
  const size_t total = count * size;
  void* ptr = arena_alloc(arena, total);
  if (ptr == NULL) {
    return NULL;
  }
  memset(ptr, 0, total);
  return ptr;
}

char* arena_strndup(struct Arena* arena, const char* str, size_t str_len) {
  // Reject `SIZE_MAX` before adding room for the terminator. `str_len + 1` would wrap to 0, which
  // yields a one-byte allocation. The terminating store below would then overrun it.
  if (str_len == SIZE_MAX) {
    return NULL;
  }
  char* copy = arena_alloc(arena, str_len + 1);
  if (copy == NULL) {
    return NULL;
  }
  // `memcpy` requires valid pointers even when the length is zero, so an empty range must skip the
  // call. The contract admits a `NULL` `str` when `str_len` is 0.
  if (str_len > 0) {
    memcpy(copy, str, str_len);
  }
  copy[str_len] = '\0';
  return copy;
}

char* arena_strdup(struct Arena* arena, const char* str) {
  return arena_strndup(arena, str, strlen(str));
}

static void* arena_alloc_chunk(struct Arena* arena, struct ArenaChunk* head, size_t size_aligned) {
  const bool is_oversized = size_aligned >= ARENA_CHUNK_CAPACITY_MIN;
  const size_t capacity = is_oversized ? size_aligned : ARENA_CHUNK_CAPACITY_MIN;
  // Check before adding the header. A wrapped total would make `malloc` succeed with far fewer than
  // `capacity` payload bytes.
  if (capacity > SIZE_MAX - sizeof(struct ArenaChunk)) {
    return NULL;
  }
  struct ArenaChunk* chunk = malloc(sizeof(*chunk) + capacity);
  if (chunk == NULL) {
    return NULL;
  }
  chunk->used = size_aligned;
  chunk->capacity = capacity;

  if (is_oversized && head != NULL) {
    // An oversized chunk has no reusable space, so the existing head must remain current.
    chunk->next = head->next;
    head->next = chunk;
  } else {
    chunk->next = head;
    arena->head = chunk;
  }
  return chunk->data;
}
