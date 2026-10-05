#include <acutest.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "formats/image.h"

/**
 * @brief Encodes one image for decoder and probe tests.
 *
 * @param image        Image to encode. Must not be `NULL`.
 * @param data_out     Receives malloc-owned JPEG bytes. Must not be `NULL`.
 * @param data_len_out Receives the JPEG byte length. Must not be `NULL`.
 */
static void encode_fixture(const struct Image* image,
                           unsigned char** data_out,
                           size_t* data_len_out) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Checks one exact orientation of the asymmetric fixture.
 *
 * @param orientation     Orientation to apply.
 * @param expected_values Expected grayscale value for each destination pixel. Must not be `NULL`.
 * @param width_expected  Expected width.
 * @param height_expected Expected height.
 */
static void check_orientation(enum ImageOrientation orientation,
                              const unsigned char* expected_values,
                              size_t width_expected,
                              size_t height_expected) __attribute__((nonnull(2)));

static void encode_fixture(const struct Image* image,
                           unsigned char** data_out,
                           size_t* data_len_out) {
  char reason[IMAGE_REASON_SIZE];
  TEST_ASSERT(image_encode_jpeg(image, 100, data_out, data_len_out, reason, sizeof(reason)) == 0);
}

static void check_orientation(enum ImageOrientation orientation,
                              const unsigned char* expected_values,
                              size_t width_expected,
                              size_t height_expected) {
  static const unsigned char source_pixels[] = {
      1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6,
  };
  struct Image oriented = {.pixels = malloc(sizeof(source_pixels)), .width_px = 2, .height_px = 3};
  TEST_ASSERT(oriented.pixels != NULL);
  if (oriented.pixels == NULL) {
    return;
  }
  memcpy(oriented.pixels, source_pixels, sizeof(source_pixels));
  char reason[IMAGE_REASON_SIZE];
  TEST_ASSERT(image_orient(&oriented, orientation, reason, sizeof(reason)) == 0);
  TEST_CHECK(oriented.width_px == width_expected);
  TEST_CHECK(oriented.height_px == height_expected);
  for (size_t i = 0; i < width_expected * height_expected; i++) {
    for (size_t channel = 0; channel < IMAGE_CHANNEL_COUNT; channel++) {
      TEST_CHECK(oriented.pixels[i * IMAGE_CHANNEL_COUNT + channel] == expected_values[i]);
    }
  }
  image_free(&oriented);
}

// A source already inside the box keeps its exact dimensions; fitting never enlarges.
static void test_image_fit_does_not_enlarge(void) {
  size_t width = 0;
  size_t height = 0;
  image_fit(100, 50, 200, 200, &width, &height);
  TEST_CHECK(width == 100);
  TEST_CHECK(height == 50);
}

// Wide and tall inputs select the limiting side while retaining aspect ratio.
static void test_image_fit_preserves_aspect_ratio(void) {
  size_t width = 0;
  size_t height = 0;
  image_fit(400, 200, 100, 100, &width, &height);
  TEST_CHECK(width == 100);
  TEST_CHECK(height == 50);

  image_fit(200, 400, 100, 100, &width, &height);
  TEST_CHECK(width == 50);
  TEST_CHECK(height == 100);

  image_fit(3, 2, 2, 2, &width, &height);
  TEST_CHECK(width == 2);
  TEST_CHECK(height == 1);
}

// An extreme aspect ratio clamps its fitted minor dimension to one pixel, never zero.
static void test_image_fit_clamps_to_one_pixel(void) {
  size_t width = 0;
  size_t height = 0;
  image_fit(IMAGE_DIMENSION_MAX, 1, 1, 1, &width, &height);
  TEST_CHECK(width == 1);
  TEST_CHECK(height == 1);
}

// Encoding produces a JPEG that the independent probe and decode entry points can consume.
static void test_image_encode_probe_and_decode(void) {
  unsigned char pixels[] = {
      20, 100, 220, 20, 100, 220, 20, 100, 220, 20, 100, 220, 20, 100, 220, 20, 100, 220,
  };
  const struct Image source = {.pixels = pixels, .width_px = 3, .height_px = 2};
  unsigned char* encoded = NULL;
  size_t encoded_len = 0;
  encode_fixture(&source, &encoded, &encoded_len);
  if (encoded == NULL) {
    return;
  }
  TEST_CHECK(encoded_len > 4);
  TEST_CHECK(encoded[0] == 0xFF && encoded[1] == 0xD8);
  TEST_CHECK(encoded[encoded_len - 2] == 0xFF && encoded[encoded_len - 1] == 0xD9);

  size_t width = 0;
  size_t height = 0;
  char reason[IMAGE_REASON_SIZE];
  TEST_CHECK(image_probe(encoded, encoded_len, &width, &height, reason, sizeof(reason)) == 0);
  TEST_CHECK(width == 3);
  TEST_CHECK(height == 2);

  struct Image decoded = {0};
  TEST_ASSERT(image_decode(encoded, encoded_len, 3, 2, &decoded, reason, sizeof(reason)) == 0);
  TEST_CHECK(decoded.width_px == 3);
  TEST_CHECK(decoded.height_px == 2);
  for (size_t i = 0; i < 6; i++) {
    TEST_CHECK(abs((int)decoded.pixels[i * 3] - 20) <= 3);
    TEST_CHECK(abs((int)decoded.pixels[i * 3 + 1] - 100) <= 3);
    TEST_CHECK(abs((int)decoded.pixels[i * 3 + 2] - 220) <= 3);
  }
  image_free(&decoded);
  free(encoded);
}

// Probing rejects hostile dimensions before the decoder can allocate their full pixel buffer.
static void test_image_probe_rejects_oversized_dimensions(void) {
  static const unsigned char jpeg[] = {
      0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x01, 0x4E, 0x21, 0x03,
      0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00, 0xFF, 0xD9,
  };
  size_t width = 0;
  size_t height = 0;
  char reason[IMAGE_REASON_SIZE];
  TEST_CHECK(image_probe(jpeg, sizeof(jpeg), &width, &height, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason, "image exceeds max dimension (20000 pixels): 20001 x 1") == 0);

  struct Image decoded = {0};
  TEST_CHECK(image_decode(jpeg, sizeof(jpeg), 1, 1, &decoded, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason, "image exceeds max dimension (20000 pixels): 20001 x 1") == 0);
  TEST_CHECK(decoded.pixels == NULL);
}

// Probing and decoding reject an input longer than stb can index before reading any of it.
static void test_image_probe_rejects_oversized_input(void) {
  static const unsigned char jpeg[] = {0xFF, 0xD8};
  const size_t data_len = (size_t)IMAGE_INPUT_LEN_MAX + 1U;
  size_t width = 0;
  size_t height = 0;
  char reason[IMAGE_REASON_SIZE];
  TEST_CHECK(image_probe(jpeg, data_len, &width, &height, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason,
                    "JPEG input exceeds max JPEG input length (2147483647 bytes) at 2147483648 "
                    "bytes") == 0);

  struct Image decoded = {0};
  TEST_CHECK(image_decode(jpeg, data_len, 1, 1, &decoded, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason,
                    "JPEG input exceeds max JPEG input length (2147483647 bytes) at 2147483648 "
                    "bytes") == 0);
  TEST_CHECK(decoded.pixels == NULL);
}

// The resize boundary uses the pinned stb v2 sRGB API and returns independently owned pixels.
static void test_image_resize_constant_color(void) {
  unsigned char pixels[] = {
      25, 100, 225, 25, 100, 225, 25, 100, 225, 25, 100, 225,
  };
  const struct Image source = {.pixels = pixels, .width_px = 2, .height_px = 2};
  struct Image resized = {0};
  char reason[IMAGE_REASON_SIZE];
  TEST_ASSERT(image_resize(&source, 1, 1, &resized, reason, sizeof(reason)) == 0);
  TEST_CHECK(resized.width_px == 1);
  TEST_CHECK(resized.height_px == 1);
  TEST_CHECK(abs((int)resized.pixels[0] - 25) <= 1);
  TEST_CHECK(abs((int)resized.pixels[1] - 100) <= 1);
  TEST_CHECK(abs((int)resized.pixels[2] - 225) <= 1);
  TEST_CHECK(resized.pixels != source.pixels);
  image_free(&resized);
}

// Center-crop geometry selects the largest rectangle with the requested aspect ratio.
static void test_image_center_crop_geometry(void) {
  size_t x = 0;
  size_t y = 0;
  size_t width = 0;
  size_t height = 0;
  image_center_crop(8, 4, 1, 1, &x, &y, &width, &height);
  TEST_CHECK(x == 2 && y == 0 && width == 4 && height == 4);

  image_center_crop(4, 8, 1, 1, &x, &y, &width, &height);
  TEST_CHECK(x == 0 && y == 2 && width == 4 && height == 4);

  image_center_crop(8, 4, 2, 1, &x, &y, &width, &height);
  TEST_CHECK(x == 0 && y == 0 && width == 8 && height == 4);
}

// A region resize honors the full source row stride while excluding pixels outside the crop.
static void test_image_resize_center_region(void) {
  unsigned char pixels[] = {
      1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4,
  };
  const struct Image source = {.pixels = pixels, .width_px = 4, .height_px = 2};
  struct Image resized = {0};
  char reason[IMAGE_REASON_SIZE];
  TEST_ASSERT(image_resize_region(&source, 1, 0, 2, 2, 2, 2, &resized, reason, sizeof(reason)) ==
              0);
  TEST_CHECK(resized.width_px == 2 && resized.height_px == 2);
  for (size_t y = 0; y < 2; y++) {
    for (size_t channel = 0; channel < IMAGE_CHANNEL_COUNT; channel++) {
      TEST_CHECK(resized.pixels[(y * 2) * IMAGE_CHANNEL_COUNT + channel] == 2);
      TEST_CHECK(resized.pixels[(y * 2 + 1) * IMAGE_CHANNEL_COUNT + channel] == 3);
    }
  }
  image_free(&resized);
}

// Each of the eight EXIF values has a distinct expected transform for a 2-by-3 image.
static void test_image_orient_all_eight_values(void) {
  static const unsigned char expected[][6] = {
      [IMAGE_ORIENTATION_TOP_LEFT] = {1, 2, 3, 4, 5, 6},
      [IMAGE_ORIENTATION_TOP_RIGHT] = {2, 1, 4, 3, 6, 5},
      [IMAGE_ORIENTATION_BOTTOM_RIGHT] = {6, 5, 4, 3, 2, 1},
      [IMAGE_ORIENTATION_BOTTOM_LEFT] = {5, 6, 3, 4, 1, 2},
      [IMAGE_ORIENTATION_LEFT_TOP] = {1, 3, 5, 2, 4, 6},
      [IMAGE_ORIENTATION_RIGHT_TOP] = {5, 3, 1, 6, 4, 2},
      [IMAGE_ORIENTATION_RIGHT_BOTTOM] = {6, 4, 2, 5, 3, 1},
      [IMAGE_ORIENTATION_LEFT_BOTTOM] = {2, 4, 6, 1, 3, 5},
  };
  for (int value = IMAGE_ORIENTATION_TOP_LEFT; value <= IMAGE_ORIENTATION_LEFT_BOTTOM; value++) {
    const enum ImageOrientation orientation = (enum ImageOrientation)value;
    const bool is_transposed = value >= IMAGE_ORIENTATION_LEFT_TOP;
    check_orientation(orientation, expected[value], is_transposed ? 3 : 2, is_transposed ? 2 : 3);
  }
}

// Orientation-size follows the same transposition rule as the pixel transform.
static void test_image_orient_size(void) {
  for (int value = IMAGE_ORIENTATION_TOP_LEFT; value <= IMAGE_ORIENTATION_BOTTOM_LEFT; value++) {
    size_t width = 0;
    size_t height = 0;
    image_orient_size((enum ImageOrientation)value, 2, 3, &width, &height);
    TEST_CHECK(width == 2);
    TEST_CHECK(height == 3);
  }
  for (int value = IMAGE_ORIENTATION_LEFT_TOP; value <= IMAGE_ORIENTATION_LEFT_BOTTOM; value++) {
    size_t width = 0;
    size_t height = 0;
    image_orient_size((enum ImageOrientation)value, 2, 3, &width, &height);
    TEST_CHECK(width == 3);
    TEST_CHECK(height == 2);
  }
}

// Top-left orientation keeps the decoded buffer, so an upright source costs no second full-size
// copy.
static void test_image_orient_top_left_keeps_buffer(void) {
  static const unsigned char source_pixels[] = {1, 2, 3};
  struct Image image = {.pixels = malloc(sizeof(source_pixels)), .width_px = 1, .height_px = 1};
  TEST_ASSERT(image.pixels != NULL);
  if (image.pixels == NULL) {
    return;
  }
  memcpy(image.pixels, source_pixels, sizeof(source_pixels));
  unsigned char* const pixels = image.pixels;
  char reason[IMAGE_REASON_SIZE];
  TEST_ASSERT(image_orient(&image, IMAGE_ORIENTATION_TOP_LEFT, reason, sizeof(reason)) == 0);
  TEST_CHECK(image.pixels == pixels);
  TEST_CHECK(image.width_px == 1);
  TEST_CHECK(image.height_px == 1);
  TEST_CHECK(memcmp(image.pixels, source_pixels, sizeof(source_pixels)) == 0);
  image_free(&image);
}

// Invalid target dimensions and quality fail at the boundary with exact diagnostics.
static void test_image_rejects_invalid_arguments(void) {
  unsigned char pixels[] = {1, 2, 3};
  const struct Image source = {.pixels = pixels, .width_px = 1, .height_px = 1};
  struct Image resized = {0};
  char reason[IMAGE_REASON_SIZE];
  TEST_CHECK(image_resize(&source, 0, 1, &resized, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason, "image dimensions must be positive: 0 x 1") == 0);
  TEST_CHECK(image_resize(&source, IMAGE_DIMENSION_MAX + 1U, 1, &resized, reason, sizeof(reason)) ==
             -1);
  TEST_CHECK(strcmp(reason, "image exceeds max dimension (20000 pixels): 20001 x 1") == 0);

  unsigned char* encoded = NULL;
  size_t encoded_len = 0;
  TEST_CHECK(image_encode_jpeg(&source, 0, &encoded, &encoded_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason, "JPEG quality must be from 1 through 100: 0") == 0);
  TEST_CHECK(encoded == NULL);
  TEST_CHECK(encoded_len == 0);
}

// Freeing is idempotent and always restores the documented zero state.
static void test_image_free_is_idempotent(void) {
  struct Image image = {.pixels = malloc(3), .width_px = 1, .height_px = 1};
  TEST_ASSERT(image.pixels != NULL);
  if (image.pixels == NULL) {
    return;
  }
  image_free(&image);
  TEST_CHECK(image.pixels == NULL);
  TEST_CHECK(image.width_px == 0);
  TEST_CHECK(image.height_px == 0);
  image_free(&image);
  TEST_CHECK(image.pixels == NULL);
}

TEST_LIST = {
    {"image fit does not enlarge", test_image_fit_does_not_enlarge},
    {"image fit preserves aspect ratio", test_image_fit_preserves_aspect_ratio},
    {"image fit clamps to one pixel", test_image_fit_clamps_to_one_pixel},
    {"image encode probe and decode", test_image_encode_probe_and_decode},
    {"image probe rejects oversized dimensions", test_image_probe_rejects_oversized_dimensions},
    {"image probe rejects oversized input", test_image_probe_rejects_oversized_input},
    {"image resize constant color", test_image_resize_constant_color},
    {"image center crop geometry", test_image_center_crop_geometry},
    {"image resize center region", test_image_resize_center_region},
    {"image orient all eight values", test_image_orient_all_eight_values},
    {"image orient size", test_image_orient_size},
    {"image orient top left keeps buffer", test_image_orient_top_left_keeps_buffer},
    {"image rejects invalid arguments", test_image_rejects_invalid_arguments},
    {"image free is idempotent", test_image_free_is_idempotent},
    {NULL, NULL},
};
