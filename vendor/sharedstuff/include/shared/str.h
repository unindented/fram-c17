#ifndef SHARED_STR_H
#define SHARED_STR_H

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

struct Arena;

/**
 * Counted byte string. `data` points at `len` bytes that need no terminator, and a null byte among
 * them is an ordinary byte.
 *
 * A string is a value: copying it copies the view, never the bytes. Whoever produced the bytes owns
 * them, usually an arena, and every view of them lives no longer than that owner. The length is
 * signed, so a subtraction that goes below zero shows up as a negative length an assertion can
 * catch instead of wrapping to a huge one.
 *
 * `data` is read-only. A producer writes the bytes through its own `char*` before wrapping them,
 * which lets `S` wrap a string literal without casting away `const`.
 */
struct Str {
  /** First byte. May be `NULL` only when `len` is 0. */
  const char* data;

  /** Number of bytes at `data`. Never negative. */
  ptrdiff_t len;
};

/**
 * @brief Expands to a braced initializer for a `struct Str` holding a string literal, without its
 *        terminator.
 *
 * This is the form for an object with static storage duration, such as a lookup table, because a
 * compound literal is not a constant expression in C17. The empty literal pasted in front makes
 * anything but a literal a compile error, because `sizeof` of a pointer would silently give the
 * wrong length.
 */
#define STR_INIT(s) {"" s, (ptrdiff_t)sizeof(s) - 1}

/** @brief Wraps a string literal, without its terminator, as a `struct Str` value. */
#define S(s) ((struct Str)STR_INIT(s))

/**
 * @brief Expands to the two arguments that a `%.*s` conversion takes for a string: its length as an
 *        `int`, then its bytes.
 *
 * Use it as `log_error(log, "failed to read '%.*s'", STR_FMT(path))`. The macro evaluates `s`
 * twice, so pass a plain variable or another lvalue. A call with no side effect, such as
 * `buf_str(&buffer)`, also works, but runs twice. `%.*s` stops at the first null byte, so a string
 * that holds one prints only the bytes before it.
 */
#define STR_FMT(s) str_fmt_len(s), str_fmt_data(s)

/**
 * @brief Returns a string's length as the `int` precision that `%.*s` takes. The function behind
 *        `STR_FMT`.
 *
 * A string longer than `INT_MAX` bytes trips an assertion, because no precision can print it.
 *
 * @param s String to measure.
 * @return `s.len`.
 */
static inline int str_fmt_len(struct Str s) {
  assert(0 <= s.len && s.len <= INT_MAX);
  return (int)s.len;
}

/**
 * @brief Returns a string's bytes as the pointer that `%.*s` takes. The function behind `STR_FMT`.
 *
 * C requires a valid pointer even for a zero precision, so an empty string with no bytes gives `""`
 * instead of `NULL`.
 *
 * @param s String to print.
 * @return `s.data`, or `""` when `s` is empty.
 */
static inline const char* str_fmt_data(struct Str s) {
  return s.len > 0 ? s.data : "";
}

/**
 * Result of `str_cut`: the bytes before the first separator, the bytes after it, and whether there
 * was one.
 */
struct StrCut {
  /** Bytes before the separator, or the whole string when there is none. */
  struct Str head;

  /** Bytes after the separator, or an empty string at the end of the input when there is none. */
  struct Str tail;

  /** Whether the separator occurs. */
  bool is_found;
};

/**
 * @brief Wraps a terminated C string, without its terminator.
 *
 * @param cstr Terminated string. Must not be `NULL`.
 * @return View of the bytes before the terminator.
 */
struct Str str_from_cstr(const char* cstr) __attribute__((nonnull(1)));

/**
 * @brief Reports whether two strings hold the same bytes.
 *
 * @param a First string.
 * @param b Second string.
 * @return `true` when the lengths and bytes match, `false` otherwise.
 */
bool str_eq(struct Str a, struct Str b);

/**
 * @brief Orders two strings byte-wise, as unsigned bytes, a prefix first.
 *
 * @param a First string.
 * @param b Second string.
 * @return A negative value when `a` sorts first, `0` when equal, a positive value otherwise.
 */
int str_compare(struct Str a, struct Str b);

/**
 * @brief Reports whether a string starts with a prefix.
 *
 * @param s      String to test.
 * @param prefix Prefix to look for.
 * @return `true` when the first `prefix.len` bytes of `s` equal `prefix`, `false` otherwise.
 */
bool str_startswith(struct Str s, struct Str prefix);

/**
 * @brief Reports whether a string ends with a suffix.
 *
 * @param s      String to test.
 * @param suffix Suffix to look for.
 * @return `true` when the last `suffix.len` bytes of `s` equal `suffix`, `false` otherwise.
 */
bool str_endswith(struct Str s, struct Str suffix);

/**
 * @brief Returns the first `n` bytes of a string.
 *
 * @param s String to take from.
 * @param n Number of bytes to keep. Must be between 0 and `s.len`.
 * @return View of `s`'s first `n` bytes.
 */
struct Str str_head(struct Str s, ptrdiff_t n);

/**
 * @brief Returns a string without its first `n` bytes.
 *
 * @param s String to cut from.
 * @param n Number of bytes to drop. Must be between 0 and `s.len`.
 * @return View of `s` after its first `n` bytes.
 */
struct Str str_tail(struct Str s, ptrdiff_t n);

/**
 * @brief Splits a string at the first occurrence of a byte.
 *
 * Calling it again on `tail` walks the fields of a delimited list.
 *
 * @param s String to split.
 * @param c Separator byte.
 * @return The bytes before and after the separator, and whether it occurs.
 */
struct StrCut str_cut(struct Str s, char c);

/**
 * @brief Copies a string's bytes into arena-owned storage.
 *
 * @param s     String to copy.
 * @param arena Arena that owns the copy. Must not be `NULL`.
 * @return The copy, the same length as `s`.
 */
struct Str str_clone(struct Str s, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Concatenates two strings, extending `head` in place when it ends where the arena's free
 *        space begins.
 *
 * Building a string by repeated concatenation onto the last allocation therefore copies each part
 * once. Otherwise `head` is copied first, and the old bytes stay valid.
 *
 * @param head  Leading bytes.
 * @param tail  Trailing bytes.
 * @param arena Arena that owns the result. Must not be `NULL`.
 * @return `head` followed by `tail`, in arena-owned storage.
 */
struct Str str_concat(struct Str head, struct Str tail, struct Arena* arena)
    __attribute__((nonnull(3)));

/**
 * @brief Copies a string into arena-owned storage with a terminator, for an interface that takes a
 *        C string.
 *
 * A null byte inside `s` ends the C string early, so the caller decides whether that can happen.
 *
 * @param s     String to copy.
 * @param arena Arena that owns the copy. Must not be `NULL`.
 * @return Terminated copy of `s`'s bytes.
 */
char* str_cstr(struct Str s, struct Arena* arena) __attribute__((nonnull(2), returns_nonnull));

#endif
