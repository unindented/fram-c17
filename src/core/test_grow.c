#include <acutest.h>
#include <stdint.h>

#include "core/grow.h"

// An empty array jumps straight to the caller's minimum capacity, and reports its byte total.
static void test_grows_from_zero_to_minimum(void) {
  size_t next = 0;
  size_t capacity_bytes = 0;
  TEST_CHECK(grow_capacity(0, 8, sizeof(int), &next, &capacity_bytes) == 0);
  TEST_CHECK(next == 8);
  TEST_CHECK(capacity_bytes == 8 * sizeof(int));
}

// A non-empty array doubles.
static void test_doubles_existing_capacity(void) {
  size_t next = 0;
  size_t capacity_bytes = 0;
  TEST_CHECK(grow_capacity(8, 8, sizeof(int), &next, &capacity_bytes) == 0);
  TEST_CHECK(next == 16);
  TEST_CHECK(capacity_bytes == 16 * sizeof(int));
  TEST_CHECK(grow_capacity(1, 8, sizeof(int), &next, &capacity_bytes) == 0);
  TEST_CHECK(next == 2);
  TEST_CHECK(capacity_bytes == 2 * sizeof(int));
}

// The reported byte total is the capacity-times-element-size product, so it is asserted against a
// non-unit element size where the two would differ.
static void test_reports_byte_total_for_wide_elements(void) {
  size_t next = 0;
  size_t capacity_bytes = 0;
  TEST_CHECK(grow_capacity(3, 8, 16, &next, &capacity_bytes) == 0);
  TEST_CHECK(next == 6);
  TEST_CHECK(capacity_bytes == 96);
}

// The largest capacity that can still double is accepted, and its successor is not.
static void test_accepts_largest_doubleable_capacity(void) {
  size_t next = 0;
  size_t capacity_bytes = 0;
  TEST_CHECK(grow_capacity(SIZE_MAX / 2, 8, 1, &next, &capacity_bytes) == 0);
  TEST_CHECK(next == (SIZE_MAX / 2) * 2);
  TEST_CHECK(capacity_bytes == (SIZE_MAX / 2) * 2);
  TEST_CHECK(grow_capacity(SIZE_MAX / 2 + 1, 8, 1, &next, &capacity_bytes) == -1);
}

// The largest byte total the product guard admits is accepted, which pins the boundary on the side
// that the rejecting sibling below cannot see: with `>=` instead of `>` this call fails, and only
// an accepting assertion notices. The expected values came from a run: `SIZE_MAX / 2` slots of 2
// bytes is `SIZE_MAX - 1` bytes, since `SIZE_MAX` is odd.
static void test_accepts_largest_byte_total(void) {
  size_t next = 0;
  size_t capacity_bytes = 0;
  TEST_CHECK(grow_capacity(0, SIZE_MAX / 2, 2, &next, &capacity_bytes) == 0);
  TEST_CHECK(next == SIZE_MAX / 2);
  TEST_CHECK(capacity_bytes == SIZE_MAX - 1);
}

// The byte-total guard is separate from the doubling guard. Both branches reach it: a capacity that
// doubles safely (`SIZE_MAX / 8`) and an empty array jumping straight to an oversized minimum
// (`capacity_min` of `SIZE_MAX`).
static void test_rejects_overflowing_byte_total(void) {
  size_t next = 0;
  size_t capacity_bytes = 0;
  TEST_CHECK(grow_capacity(SIZE_MAX / 8, 8, 8, &next, &capacity_bytes) == -1);
  TEST_CHECK(grow_capacity(0, SIZE_MAX, 2, &next, &capacity_bytes) == -1);
}

// A zero element size is rejected rather than dividing by it. Every caller passes a `sizeof`, so
// this is unreachable today. It is pinned because the guard's only job is to keep the
// `SIZE_MAX / elem_size` division defined. A later reader would otherwise read it as dead.
static void test_rejects_zero_element_size(void) {
  size_t next = 0;
  size_t capacity_bytes = 0;
  TEST_CHECK(grow_capacity(0, 8, 0, &next, &capacity_bytes) == -1);
  TEST_CHECK(grow_capacity(8, 8, 0, &next, &capacity_bytes) == -1);
}

// A zero minimum is rejected, since an empty array jumping to it would report success with no slot
// for the append that asked it to grow. The rejection does not depend on the current capacity.
static void test_rejects_zero_minimum(void) {
  size_t next = 0;
  size_t capacity_bytes = 0;
  TEST_CHECK(grow_capacity(0, 0, sizeof(int), &next, &capacity_bytes) == -1);
  TEST_CHECK(grow_capacity(8, 0, sizeof(int), &next, &capacity_bytes) == -1);
}

TEST_LIST = {
    {"grows from zero to minimum", test_grows_from_zero_to_minimum},
    {"doubles existing capacity", test_doubles_existing_capacity},
    {"reports byte total for wide elements", test_reports_byte_total_for_wide_elements},
    {"accepts largest doubleable capacity", test_accepts_largest_doubleable_capacity},
    {"accepts largest byte total", test_accepts_largest_byte_total},
    {"rejects overflowing byte total", test_rejects_overflowing_byte_total},
    {"rejects zero element size", test_rejects_zero_element_size},
    {"rejects zero minimum", test_rejects_zero_minimum},
    {NULL, NULL},
};
