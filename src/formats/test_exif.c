#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "formats/exif.h"

enum { EXIF_APP1_END = 38, EXIF_FIXTURE_SIZE = 40 };

/**
 * @brief Builds a minimal JPEG containing one EXIF orientation entry.
 *
 * @param bytes            Destination fixture. Must not be `NULL`.
 * @param is_little_endian Whether TIFF integers use little endian order.
 * @param orientation      Raw orientation value to store.
 */
static void build_exif(unsigned char bytes[static EXIF_FIXTURE_SIZE],
                       bool is_little_endian,
                       unsigned char orientation);

static void build_exif(unsigned char bytes[static EXIF_FIXTURE_SIZE],
                       bool is_little_endian,
                       unsigned char orientation) {
  static const unsigned char prefix[] = {
      0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0x00, 0x00,
  };
  memset(bytes, 0, EXIF_FIXTURE_SIZE);
  memcpy(bytes, prefix, sizeof(prefix));
  unsigned char* tiff = bytes + sizeof(prefix);
  if (is_little_endian) {
    const unsigned char body[] = {
        'I',  'I',  0x2A, 0x00, 0x08, 0x00,        0x00, 0x00, 0x01, 0x00, 0x12, 0x01, 0x03,
        0x00, 0x01, 0x00, 0x00, 0x00, orientation, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(tiff, body, sizeof(body));
  } else {
    const unsigned char body[] = {
        'M',  'M',  0x00, 0x2A, 0x00, 0x00, 0x00,        0x08, 0x00, 0x01, 0x01, 0x12, 0x00,
        0x03, 0x00, 0x00, 0x00, 0x01, 0x00, orientation, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(tiff, body, sizeof(body));
  }
  bytes[38] = 0xFF;
  bytes[39] = 0xD9;
}

// All eight standard values are returned without remapping.
static void test_exif_reads_all_orientations(void) {
  for (int value = IMAGE_ORIENTATION_TOP_LEFT; value <= IMAGE_ORIENTATION_LEFT_BOTTOM; value++) {
    unsigned char jpeg[EXIF_FIXTURE_SIZE];
    build_exif(jpeg, true, (unsigned char)value);
    TEST_CHECK(exif_read_orientation(jpeg, sizeof(jpeg)) == (enum ImageOrientation)value);
  }
}

// TIFF integers are decoded in both byte orders.
static void test_exif_reads_both_byte_orders(void) {
  unsigned char little[EXIF_FIXTURE_SIZE];
  unsigned char big[EXIF_FIXTURE_SIZE];
  build_exif(little, true, IMAGE_ORIENTATION_RIGHT_TOP);
  build_exif(big, false, IMAGE_ORIENTATION_LEFT_BOTTOM);
  TEST_CHECK(exif_read_orientation(little, sizeof(little)) == IMAGE_ORIENTATION_RIGHT_TOP);
  TEST_CHECK(exif_read_orientation(big, sizeof(big)) == IMAGE_ORIENTATION_LEFT_BOTTOM);
}

// Non-EXIF segments are skipped before a later APP1 segment is parsed.
static void test_exif_skips_other_segments(void) {
  unsigned char exif[EXIF_FIXTURE_SIZE];
  build_exif(exif, true, IMAGE_ORIENTATION_BOTTOM_LEFT);
  unsigned char jpeg[EXIF_FIXTURE_SIZE + 6] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0xAA, 0xBB};
  memcpy(jpeg + 8, exif + 2, sizeof(exif) - 2);
  TEST_CHECK(exif_read_orientation(jpeg, sizeof(jpeg)) == IMAGE_ORIENTATION_BOTTOM_LEFT);
}

// A JPEG with no APP1 metadata has the normal top-left presentation.
static void test_exif_defaults_when_absent(void) {
  static const unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xD9};
  static const unsigned char not_jpeg[] = {0x00, 0x01, 0x02};
  TEST_CHECK(exif_read_orientation(jpeg, sizeof(jpeg)) == IMAGE_ORIENTATION_TOP_LEFT);
  TEST_CHECK(exif_read_orientation(not_jpeg, sizeof(not_jpeg)) == IMAGE_ORIENTATION_TOP_LEFT);
}

// Truncation at every byte boundary is bounded and defaults instead of reading beyond the buffer.
static void test_exif_defaults_on_truncated_app1(void) {
  unsigned char jpeg[EXIF_FIXTURE_SIZE];
  build_exif(jpeg, true, IMAGE_ORIENTATION_RIGHT_BOTTOM);
  // The APP1 segment ends at byte 38. Omitting only the optional EOI marker does not truncate the
  // metadata and must not invalidate an otherwise complete orientation.
  for (size_t len = 0; len < EXIF_APP1_END; len++) {
    TEST_CHECK(exif_read_orientation(jpeg, len) == IMAGE_ORIENTATION_TOP_LEFT);
  }
}

// A zero segment length is rejected, so the segment walk cannot stall or wrap.
static void test_exif_defaults_on_zero_segment_length(void) {
  unsigned char jpeg[EXIF_FIXTURE_SIZE];
  build_exif(jpeg, true, IMAGE_ORIENTATION_RIGHT_TOP);
  jpeg[4] = 0;
  jpeg[5] = 0;
  TEST_CHECK(exif_read_orientation(jpeg, sizeof(jpeg)) == IMAGE_ORIENTATION_TOP_LEFT);
}

// An IFD offset outside the APP1 payload is rejected before pointer arithmetic reaches it.
static void test_exif_defaults_on_out_of_range_ifd(void) {
  unsigned char little[EXIF_FIXTURE_SIZE];
  build_exif(little, true, IMAGE_ORIENTATION_RIGHT_TOP);
  little[16] = 0xFF;
  little[17] = 0xFF;
  little[18] = 0xFF;
  little[19] = 0x7F;
  TEST_CHECK(exif_read_orientation(little, sizeof(little)) == IMAGE_ORIENTATION_TOP_LEFT);

  unsigned char big[EXIF_FIXTURE_SIZE];
  build_exif(big, false, IMAGE_ORIENTATION_RIGHT_TOP);
  big[16] = 0x7F;
  big[17] = 0xFF;
  big[18] = 0xFF;
  big[19] = 0xFF;
  TEST_CHECK(exif_read_orientation(big, sizeof(big)) == IMAGE_ORIENTATION_TOP_LEFT);
}

// Wrong TIFF type/count combinations and out-of-range values are not usable orientations.
static void test_exif_defaults_on_invalid_orientation_entry(void) {
  unsigned char jpeg[EXIF_FIXTURE_SIZE];
  build_exif(jpeg, true, 9);
  TEST_CHECK(exif_read_orientation(jpeg, sizeof(jpeg)) == IMAGE_ORIENTATION_TOP_LEFT);

  build_exif(jpeg, true, IMAGE_ORIENTATION_RIGHT_TOP);
  jpeg[24] = 0x04;
  TEST_CHECK(exif_read_orientation(jpeg, sizeof(jpeg)) == IMAGE_ORIENTATION_TOP_LEFT);

  build_exif(jpeg, true, IMAGE_ORIENTATION_RIGHT_TOP);
  jpeg[26] = 0x02;
  TEST_CHECK(exif_read_orientation(jpeg, sizeof(jpeg)) == IMAGE_ORIENTATION_TOP_LEFT);
}

TEST_LIST = {
    {"exif reads all orientations", test_exif_reads_all_orientations},
    {"exif reads both byte orders", test_exif_reads_both_byte_orders},
    {"exif skips other segments", test_exif_skips_other_segments},
    {"exif defaults when absent", test_exif_defaults_when_absent},
    {"exif defaults on truncated app1", test_exif_defaults_on_truncated_app1},
    {"exif defaults on zero segment length", test_exif_defaults_on_zero_segment_length},
    {"exif defaults on out of range ifd", test_exif_defaults_on_out_of_range_ifd},
    {"exif defaults on invalid orientation entry", test_exif_defaults_on_invalid_orientation_entry},
    {NULL, NULL},
};
