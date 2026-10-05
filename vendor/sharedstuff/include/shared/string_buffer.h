#ifndef SHARED_STRING_BUFFER_H
#define SHARED_STRING_BUFFER_H

#include <stddef.h>

/** Growable string buffer. */
struct StringBuffer {
  /** Heap allocation containing `len` bytes plus a `NUL` terminator when non-`NULL`. */
  char* data;

  /** Number of meaningful bytes currently stored in `data`. */
  size_t len;

  /** Allocated bytes in `data`. */
  size_t capacity;
};

/**
 * @brief Initializes an empty string buffer with no heap allocation.
 *
 * @param buffer Buffer handle to prepare. Must not be `NULL`.
 */
void string_buffer_init(struct StringBuffer* buffer) __attribute__((nonnull(1)));

/**
 * @brief Releases the buffer allocation and resets it for reuse.
 *
 * Leaves the buffer initialized, so it may be reused without `string_buffer_init`.
 *
 * @param buffer Buffer to release. Must not be `NULL`.
 */
void string_buffer_free(struct StringBuffer* buffer) __attribute__((nonnull(1)));

/**
 * @brief Ensures the buffer has room for `extra_len` more bytes plus a `NUL` terminator.
 *
 * Grows capacity geometrically when needed. It preserves existing contents. On success the buffer
 * is `NUL`-terminated at `len`, so a reserved-but-not-yet-appended buffer is a valid C string.
 *
 * A growth reallocates `data`, so any pointer into the buffer taken before this call is dangling
 * once it returns. A read or write through one is undefined behavior. Re-read `buffer->data` after
 * every reserve or append rather than holding a pointer across one.
 *
 * @param buffer    Buffer to grow. Must not be `NULL`.
 * @param extra_len Additional byte count the caller intends to append.
 * @return `0` on success, or `-1` on size overflow or allocation failure.
 */
int string_buffer_reserve(struct StringBuffer* buffer, size_t extra_len)
    __attribute__((nonnull(1)));

/**
 * @brief Appends a byte range and maintains a `NUL` terminator.
 *
 * @param buffer  Buffer to append to. Must not be `NULL`.
 * @param str     Source bytes. Must hold at least `str_len` bytes. May be `NULL` only when
 *                `str_len` is 0, and must not point into `buffer->data`. A source inside the
 *                destination is undefined for two reasons: `memcpy` forbids overlapping objects,
 *                and growth would free the region `str` points at before the copy runs.
 * @param str_len Number of bytes to append.
 * @return `0` on success, or `-1` on size overflow or allocation failure (the buffer is left
 *         unchanged).
 */
int string_buffer_append_len(struct StringBuffer* buffer, const char* str, size_t str_len)
    __attribute__((nonnull(1)));

/**
 * @brief Appends a terminated string.
 *
 * @param buffer Buffer to append to. Must not be `NULL`.
 * @param str    Terminated source string. Must not be `NULL`.
 * @return `0` on success, or `-1` on size overflow or allocation failure.
 */
int string_buffer_append(struct StringBuffer* buffer, const char* str)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Appends a single byte.
 *
 * @param buffer Buffer to append to. Must not be `NULL`.
 * @param c      Byte to append.
 * @return `0` on success, or `-1` on size overflow or allocation failure.
 */
int string_buffer_append_char(struct StringBuffer* buffer, char c) __attribute__((nonnull(1)));

/**
 * @brief Transfers the heap buffer to the caller and resets the buffer.
 *
 * Ownership of the returned allocation passes to the caller, who must `free` it. The buffer is
 * reset to empty and may be reused. A buffer that never grew yields a terminated empty string
 * rather than `NULL`, so callers need not special-case it.
 *
 * @param buffer Buffer to steal from. Must not be `NULL`.
 * @return The terminated heap allocation, or `NULL` on allocation failure.
 */
char* string_buffer_steal(struct StringBuffer* buffer) __attribute__((nonnull(1)));

#endif
