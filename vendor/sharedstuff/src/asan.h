#ifndef SHARED_ASAN_H
#define SHARED_ASAN_H

// GCC and newer Clang define `__SANITIZE_ADDRESS__` under `-fsanitize=address`. Apple Clang does
// not, but answers `__has_feature(address_sanitizer)`. cppcheck evaluates the second test even
// where `__has_feature` does not exist and fails on it, so the test is hidden from it.
#if defined(__SANITIZE_ADDRESS__)
#define SHARED_ASAN 1
#elif defined(__has_feature) && !defined(__CPPCHECK__)
#if __has_feature(address_sanitizer)
#define SHARED_ASAN 1
#endif
#endif

#ifndef SHARED_ASAN
/** Whether this translation unit is built with the address sanitizer: `1` or `0`. */
#define SHARED_ASAN 0
#endif

#if SHARED_ASAN
#include <sanitizer/asan_interface.h>
#endif

#endif
