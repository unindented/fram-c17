#ifndef FRAM_ASCII_H
#define FRAM_ASCII_H

#include <stdbool.h>

// This is hand-rolled ASCII classification rather than `<ctype.h>`, for two reasons. `<ctype.h>` is
// locale-dependent, so under a non-C locale `isalnum` and `tolower` can accept or fold bytes
// outside ASCII. A generated slug or output path would then depend on the environment the build ran
// in. These answer for ASCII only, whatever the locale. Every `<ctype.h>` function also takes an
// `int` whose value must be representable as an `unsigned char` or equal `EOF`. If plain `char` is
// signed, passing a byte above 0x7F causes undefined behavior. The byte becomes a negative value
// that is outside the permitted range. Plain `char` is signed on two of the three release targets,
// x86-64 Linux and arm64 macOS. Arm64 Linux has it unsigned. Taking `unsigned char` moves that
// conversion into the parameter, where it is value-preserving, so a caller scanning with a
// `const unsigned char*` or an explicit `(unsigned char)` cast cannot get it wrong. `isspace` is
// locale-dependent the same way, so a parsed video duration uses `ascii_is_space` instead.
//
// These are header-only and `static inline`, because every body is one or two comparisons.

/**
 * @brief Reports whether `c` is an ASCII decimal digit.
 *
 * @param c Byte to classify.
 * @return `true` when `c` is in `[0-9]`, `false` otherwise.
 */
static inline bool ascii_is_digit(unsigned char c) {
  return c >= '0' && c <= '9';
}

/**
 * @brief Reports whether `c` is an ASCII letter or digit.
 *
 * @param c Byte to classify.
 * @return `true` when `c` is in `[0-9A-Za-z]`, `false` otherwise.
 */
static inline bool ascii_is_alphanumeric(unsigned char c) {
  return ascii_is_digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/**
 * @brief Reports whether `c` is ASCII whitespace.
 *
 * @param c Byte to classify.
 * @return `true` when `c` is a space, `\t`, `\n`, `\v`, `\f`, or `\r`, `false` otherwise.
 */
static inline bool ascii_is_space(unsigned char c) {
  return c == ' ' || (c >= '\t' && c <= '\r');
}

/**
 * @brief Lowercases an ASCII uppercase letter, leaving every other byte unchanged.
 *
 * @param c Byte to convert.
 * @return `c` mapped to lowercase when it is in `[A-Z]`, otherwise `c` unchanged.
 */
static inline unsigned char ascii_to_lower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

#endif
