#ifndef FRAM_TEST_JPEG_H
#define FRAM_TEST_JPEG_H

#include <stddef.h>
#include <stdlib.h>

#include "formats/image.h"

// This is header-only and `static inline` so that every unit test encoding a fixture JPEG can
// include it without a separate test library to link.

/**
 * @brief Encodes an RGB JPEG fixture of the given size with a fixed, non-uniform pixel pattern.
 *
 * The pattern is asymmetric, so an orientation or crop mistake changes the encoded result.
 *
 * @param width_px     Fixture width in pixels. Must be positive.
 * @param height_px    Fixture height in pixels. Must be positive.
 * @param data_out     Receives the encoded JPEG, which the caller must `free`. Must not be `NULL`.
 * @param data_len_out Receives the encoded length in bytes. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation or encoding failure.
 */
static inline int test_jpeg_encode(size_t width_px,
                                   size_t height_px,
                                   unsigned char** data_out,
                                   size_t* data_len_out) __attribute__((nonnull(3, 4)));

static inline int test_jpeg_encode(size_t width_px,
                                   size_t height_px,
                                   unsigned char** data_out,
                                   size_t* data_len_out) {
  const size_t pixel_len = width_px * height_px * IMAGE_CHANNEL_COUNT;
  unsigned char* pixels = malloc(pixel_len);
  if (pixels == NULL) {
    return -1;
  }
  for (size_t i = 0; i < pixel_len; i++) {
    pixels[i] = (unsigned char)((i * 37) % 251);
  }
  const struct Image image = {.pixels = pixels, .width_px = width_px, .height_px = height_px};
  const int rc = image_encode_jpeg(&image, 90, data_out, data_len_out, NULL, 0);
  free(pixels);
  return rc;
}

#endif
