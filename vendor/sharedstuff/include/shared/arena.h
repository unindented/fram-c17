#ifndef SHARED_ARENA_H
#define SHARED_ARENA_H

#include <stddef.h>

/** Heap chunk of bump-allocated storage. */
struct ArenaChunk;

/** Handle for a group of allocations released together. */
struct Arena {
  /** Head of the arena's chunk list. */
  struct ArenaChunk* head;
};

/**
 * @brief Initializes an arena before first use.
 *
 * @param arena Arena handle to prepare. Must not be `NULL`.
 */
void arena_init(struct Arena* arena) __attribute__((nonnull(1)));

/**
 * @brief Releases every allocation owned by the arena and resets it for reuse.
 *
 * Invalidates every pointer previously returned by the arena's allocators. The arena stays
 * initialized, so it may be reused without calling `arena_init`.
 *
 * @param arena Arena to release. Must not be `NULL`.
 */
void arena_free(struct Arena* arena) __attribute__((nonnull(1)));

/**
 * @brief Allocates `size` bytes of arena-owned storage.
 *
 * The storage is aligned for any type and stays valid until `arena_free`. A request of zero bytes
 * still returns a distinct, usable one-byte allocation.
 *
 * @param arena Arena that owns the allocation. Must not be `NULL`.
 * @param size  Number of bytes to allocate.
 * @return Pointer to uninitialized storage, or `NULL` on size overflow or allocation failure.
 */
void* arena_alloc(struct Arena* arena, size_t size) __attribute__((nonnull(1)));

/**
 * @brief Allocates zero-filled arena-owned storage for `count` elements of `size` bytes each.
 *
 * @param arena Arena that owns the allocation. Must not be `NULL`.
 * @param count Number of elements.
 * @param size  Size in bytes of one element.
 * @return Pointer to zeroed storage, or `NULL` when `count * size` overflows or allocation fails.
 */
void* arena_calloc(struct Arena* arena, size_t count, size_t size) __attribute__((nonnull(1)));

/**
 * @brief Copies a byte range into arena-owned storage and appends a `NUL` terminator.
 *
 * @param arena   Arena that owns the copy. Must not be `NULL`.
 * @param str     Source bytes to copy. Must hold at least `str_len` bytes. May be `NULL` only when
 *                `str_len` is 0.
 * @param str_len Number of bytes to copy, excluding any terminator.
 * @return Terminated copy owned by the arena, or `NULL` on size overflow (`str_len == SIZE_MAX`) or
 *         allocation failure.
 */
char* arena_strndup(struct Arena* arena, const char* str, size_t str_len)
    __attribute__((nonnull(1)));

/**
 * @brief Copies a terminated string into arena-owned storage.
 *
 * @param arena Arena that owns the copy. Must not be `NULL`.
 * @param str   Terminated source string to copy. Must not be `NULL`.
 * @return Terminated copy owned by the arena, or `NULL` on allocation failure.
 */
char* arena_strdup(struct Arena* arena, const char* str) __attribute__((nonnull(1, 2)));

#endif
