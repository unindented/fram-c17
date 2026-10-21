#include "shared/str.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "shared/arena.h"

struct Str str_from_cstr(const char* cstr) {
  return (struct Str){cstr, (ptrdiff_t)strlen(cstr)};
}

bool str_eq(struct Str a, struct Str b) {
  // `memcmp` requires valid pointers even for zero bytes, and an empty string may hold `NULL`.
  return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, (size_t)a.len) == 0);
}

int str_compare(struct Str a, struct Str b) {
  const ptrdiff_t len = a.len < b.len ? a.len : b.len;
  const int order = len == 0 ? 0 : memcmp(a.data, b.data, (size_t)len);
  if (order != 0) {
    return order;
  }
  return (a.len > b.len) - (a.len < b.len);
}

bool str_startswith(struct Str s, struct Str prefix) {
  return s.len >= prefix.len && str_eq(str_head(s, prefix.len), prefix);
}

bool str_endswith(struct Str s, struct Str suffix) {
  return s.len >= suffix.len && str_eq(str_tail(s, s.len - suffix.len), suffix);
}

struct Str str_head(struct Str s, ptrdiff_t n) {
  assert(n >= 0 && n <= s.len);
  s.len = n;
  return s;
}

struct Str str_tail(struct Str s, ptrdiff_t n) {
  assert(n >= 0 && n <= s.len);
  // Offsetting a `NULL` pointer is undefined even by zero, and an empty string may hold `NULL`.
  if (n > 0) {
    s.data += n;
    s.len -= n;
  }
  return s;
}

struct StrCut str_cut(struct Str s, char c) {
  ptrdiff_t i = 0;
  while (i < s.len && s.data[i] != c) {
    i++;
  }
  const bool is_found = i < s.len;
  return (struct StrCut){
      .head = str_head(s, i),
      .tail = str_tail(s, i + is_found),
      .is_found = is_found,
  };
}

struct Str str_clone(struct Str s, struct Arena* arena) {
  char* data = arena_new(arena, s.len, char);
  // `memcpy` requires valid pointers even for zero bytes, and an empty string may hold `NULL`.
  if (s.len > 0) {
    memcpy(data, s.data, (size_t)s.len);
  }
  return (struct Str){data, s.len};
}

struct Str str_concat(struct Str head, struct Str tail, struct Arena* arena) {
  if (head.len == 0 || head.data + head.len != arena->beg) {
    head = str_clone(head, arena);
  }
  // `head` now ends where the free space begins, so the extension lands right after it.
  char* data = arena_extend(arena, tail.len, 1);
  // `memcpy` requires valid pointers even for zero bytes, and an empty string may hold `NULL`.
  if (tail.len > 0) {
    memcpy(data, tail.data, (size_t)tail.len);
  }
  head.len += tail.len;
  return head;
}

char* str_cstr(struct Str s, struct Arena* arena) {
  // The arena zeroes the allocation, so the byte after the copy is already the terminator.
  char* cstr = arena_new(arena, s.len + 1, char);
  if (s.len > 0) {
    memcpy(cstr, s.data, (size_t)s.len);
  }
  return cstr;
}
