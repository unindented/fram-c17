#ifndef FRAM_TEST_JPEG_H
#define FRAM_TEST_JPEG_H

#include <stddef.h>
#include <stdlib.h>

#include "formats/image.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// This is header-only and `static inline` so that every unit test encoding a fixture JPEG can
// include it without a separate test library to link. It records a failure with acutest's
// `TEST_MSG`, so a test includes `<acutest.h>` before it.

/**
 * @brief Encodes an RGB JPEG fixture of the given size with a fixed, non-uniform pixel pattern,
 *        recording why when it cannot.
 *
 * The pattern is asymmetric, so an orientation or crop mistake changes the encoded result.
 *
 * @param width_px  Fixture width in pixels. Must be positive.
 * @param height_px Fixture height in pixels. Must be positive.
 * @param arena     Arena that receives the encoded bytes. Must not be `NULL`.
 * @param jpeg_out  Receives the encoded JPEG, which lives as long as `arena`. Must not be `NULL`.
 * @return `0` on success, or `-1` on an encoding failure.
 */
static inline int test_jpeg_encode(ptrdiff_t width_px,
                                   ptrdiff_t height_px,
                                   struct Arena* arena,
                                   struct Str* jpeg_out) __attribute__((nonnull(3, 4)));

static inline int test_jpeg_encode(ptrdiff_t width_px,
                                   ptrdiff_t height_px,
                                   struct Arena* arena,
                                   struct Str* jpeg_out) {
  const ptrdiff_t pixel_len = width_px * height_px * IMAGE_CHANNEL_COUNT;
  unsigned char* pixels = image_alloc((size_t)pixel_len);
  for (ptrdiff_t i = 0; i < pixel_len; i++) {
    pixels[i] = (unsigned char)((i * 37) % 251);
  }
  const struct Image image = {.pixels = pixels, .width_px = width_px, .height_px = height_px};
  struct Log log = test_log();
  const int rc = image_encode_jpeg(&image, 90, arena, jpeg_out, S("failed to encode fixture"),
                                   S("fixture.jpg"), &log);
  free(pixels);
  if (rc != 0) {
    const struct Str failure = buf_str(&log.buf);
    TEST_MSG("%.*s", STR_FMT(failure));
    return -1;
  }
  return 0;
}

#endif
