#include <acutest.h>
#include <stddef.h>
#include <stdio.h>

#include "core/path_list.h"
#include "shared/arena.h"
#include "shared/str.h"
#include "test_support.h"

// Pushing well past the first capacity grows the backing array while preserving every path. Each
// stored path is an independent copy of the caller's buffer. `arena_grow` seeds an empty array with
// 8 slots, file-local to `vendor/sharedstuff/src/arena.c`, so it is spelled out here. Raising it
// there must raise `PUSH_COUNT`, or this stops forcing a second growth. The exact final capacity is
// asserted rather than a lower bound, so a change to either the seed or the doubling fails here.
static void test_push_grows_and_copies(void) {
  struct Arena arena = test_arena();
  struct PathList list = {0};

  enum { CAPACITY_MIN_ASSUMED = 8, PUSH_COUNT = CAPACITY_MIN_ASSUMED * 2 + 4 };
  char scratch[32];
  for (int i = 0; i < PUSH_COUNT; i++) {
    const int scratch_len = snprintf(scratch, sizeof(scratch), "album/media-%d.jpg", i);
    TEST_ASSERT(scratch_len > 0 && scratch_len < (int)sizeof(scratch));
    path_list_push(&list, (struct Str){scratch, scratch_len}, &arena);
  }
  TEST_CHECK(list.count == PUSH_COUNT);
  // 8 -> 16 at push 9, 16 -> 32 at push 17.
  TEST_CHECK(list.capacity == CAPACITY_MIN_ASSUMED * 4);

  for (int i = 0; i < PUSH_COUNT; i++) {
    char expected[32];
    const int expected_len = snprintf(expected, sizeof(expected), "album/media-%d.jpg", i);
    TEST_ASSERT(expected_len > 0 && expected_len < (int)sizeof(expected));
    TEST_CHECK(str_eq(list.items[i], (struct Str){expected, expected_len}));
    TEST_CHECK(list.items[i].data != scratch);  // stored value is a copy, not an alias
  }
}

TEST_LIST = {
    {"push grows and copies", test_push_grows_and_copies},
    {NULL, NULL},
};
