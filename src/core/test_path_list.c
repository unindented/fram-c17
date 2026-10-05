#include <acutest.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core/path_list.h"

// A freshly initialized list is empty and holds no allocation.
static void test_init_is_empty(void) {
  struct PathList list;
  path_list_init(&list);
  TEST_CHECK(list.count == 0);
  TEST_CHECK(list.capacity == 0);
  TEST_CHECK(list.items == NULL);
  path_list_free(&list);
}

// `path_list_free` leaves the list reusable without a second `path_list_init`.
static void test_free_allows_reuse(void) {
  struct PathList list;
  path_list_init(&list);
  TEST_ASSERT(path_list_push(&list, "a.jpg") == 0);
  path_list_free(&list);

  TEST_ASSERT(path_list_push(&list, "b.jpg") == 0);
  TEST_CHECK(list.count == 1);
  TEST_CHECK(strcmp(list.items[0], "b.jpg") == 0);
  path_list_free(&list);
}

// Pushing well past the initial capacity grows the backing array while preserving every path. Each
// stored path is an independent copy of the caller's buffer. `PATH_LIST_CAPACITY_MIN` is 16,
// file-local to `src/core/path_list.c`, so it is spelled out here. Raising it there must raise
// `PUSH_COUNT`, or this stops forcing the *second* growth. A single growth cannot distinguish a
// correct doubling policy from one that recomputes the same capacity. The exact final capacity is
// asserted rather than a lower bound, so a change to either the seed or the doubling fails here
// instead of silently defeating the test.
static void test_push_grows_and_copies(void) {
  struct PathList list;
  path_list_init(&list);

  enum { CAPACITY_MIN_ASSUMED = 16, PUSH_COUNT = CAPACITY_MIN_ASSUMED * 2 + 8 };
  char scratch[32];
  for (int i = 0; i < PUSH_COUNT; i++) {
    const int scratch_len = snprintf(scratch, sizeof(scratch), "album/media-%d.jpg", i);
    TEST_ASSERT(scratch_len > 0 && (size_t)scratch_len < sizeof(scratch));
    TEST_ASSERT(path_list_push(&list, scratch) == 0);
  }
  TEST_CHECK(list.count == PUSH_COUNT);
  // 16 -> 32 at push 17, 32 -> 64 at push 33.
  TEST_CHECK(list.capacity == CAPACITY_MIN_ASSUMED * 4);

  for (int i = 0; i < PUSH_COUNT; i++) {
    char expected[32];
    const int expected_len = snprintf(expected, sizeof(expected), "album/media-%d.jpg", i);
    TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(strcmp(list.items[i], expected) == 0);
    TEST_CHECK(list.items[i] != scratch);  // stored value is a copy, not an alias
  }

  path_list_free(&list);
  TEST_CHECK(list.count == 0);
  TEST_CHECK(list.items == NULL);
}

TEST_LIST = {
    {"init is empty", test_init_is_empty},
    {"free allows reuse", test_free_allows_reuse},
    {"push grows and copies", test_push_grows_and_copies},
    {NULL, NULL},
};
