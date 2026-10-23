#ifndef FRAM_TEST_JPEG_FIXTURE_H
#define FRAM_TEST_JPEG_FIXTURE_H

#include <stddef.h>

#include "formats/image.h"
#include "shared/arena.h"
#include "shared/str.h"

/** Color space and scan structure of a fixture JPEG. */
enum TestJpegKind {
  /** YCbCr with subsampled chroma, in one sequential scan, as a camera writes it. */
  TEST_JPEG_BASELINE,

  /** YCbCr with subsampled chroma, in progressive scans, which a decoder buffers whole. */
  TEST_JPEG_PROGRESSIVE,

  /** Adobe-style inverted CMYK. */
  TEST_JPEG_CMYK,

  /** Adobe-style inverted CMYK, stored as YCCK. */
  TEST_JPEG_YCCK,

  /** Lossless RGB, which libjpeg-turbo decodes only at full size. */
  TEST_JPEG_LOSSLESS,
};

/**
 * @brief Encodes an RGB image as a fixture JPEG of one kind, recording why when it cannot.
 *
 * The encoder is libjpeg-turbo's, the same library `image_decode` uses, so a fixture exercises
 * exactly the JPEG features its kind names. A CMYK or YCCK fixture stores each pixel so that its
 * colors multiplied by its black, over 255, give back the RGB value. A failure is recorded with
 * acutest's `TEST_MSG`.
 *
 * @param image    Image to encode, with valid bounded dimensions and pixels. Must not be `NULL`.
 * @param kind     Color space and scan structure to encode with.
 * @param quality  JPEG quality from 1 through 100. Unused for a lossless fixture.
 * @param arena    Arena that receives the encoded bytes. Must not be `NULL`.
 * @param jpeg_out Receives the encoded JPEG, which lives as long as `arena`. Must not be `NULL`.
 * @return `0` on success, or `-1` on an encoding failure.
 */
int test_jpeg_encode_image(const struct Image* image,
                           enum TestJpegKind kind,
                           ptrdiff_t quality,
                           struct Arena* arena,
                           struct Str* jpeg_out) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Encodes a fixture of one kind, every pixel one RGB color, at quality 95, recording why
 *        when it cannot.
 *
 * @param width_px  Fixture width in pixels. Must be positive.
 * @param height_px Fixture height in pixels. Must be positive.
 * @param color     RGB color of every pixel. Must not be `NULL`.
 * @param kind      Color space and scan structure to encode with.
 * @param arena     Arena that receives the encoded bytes. Must not be `NULL`.
 * @param jpeg_out  Receives the encoded JPEG, which lives as long as `arena`. Must not be `NULL`.
 * @return `0` on success, or `-1` on an encoding failure.
 */
int test_jpeg_encode_solid(ptrdiff_t width_px,
                           ptrdiff_t height_px,
                           const unsigned char* color,
                           enum TestJpegKind kind,
                           struct Arena* arena,
                           struct Str* jpeg_out) __attribute__((nonnull(3, 5, 6)));

#endif
