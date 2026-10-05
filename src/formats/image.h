#ifndef FRAM_IMAGE_H
#define FRAM_IMAGE_H

#include <limits.h>
#include <stddef.h>

/** Number of interleaved color channels in every decoded image. */
enum { IMAGE_CHANNEL_COUNT = 3 };

/** Largest accepted width or height, in pixels. */
enum { IMAGE_DIMENSION_MAX = 20000 };

/** Largest accepted width-times-height product, in pixels. */
enum { IMAGE_PIXEL_COUNT_MAX = 100000000 };

/**
 * Longest encoded input stb can take, in bytes. stb addresses its input with an `int`. A caller
 * that reads a JPEG from disk passes this bound to `fs_read_file`, so an oversize file is rejected
 * from its size before it is loaded.
 */
enum { IMAGE_INPUT_LEN_MAX = INT_MAX };

/** Largest JPEG output the encoder callback may retain, in bytes. */
enum { IMAGE_ENCODED_LEN_MAX = 512 * 1024 * 1024 };

/** Size of an image-operation diagnostic, including the `NUL` terminator. */
enum { IMAGE_REASON_SIZE = 256 };

/** TIFF/EXIF orientation values, named for the stored image's first row and column. */
enum ImageOrientation {
  /** Stored upright, so orienting changes nothing. Also the fallback when none is found. */
  IMAGE_ORIENTATION_TOP_LEFT = 1,

  /** Corrected by mirroring horizontally. */
  IMAGE_ORIENTATION_TOP_RIGHT = 2,

  /** Corrected by rotating 180 degrees. */
  IMAGE_ORIENTATION_BOTTOM_RIGHT = 3,

  /** Corrected by mirroring vertically. */
  IMAGE_ORIENTATION_BOTTOM_LEFT = 4,

  /** Corrected by mirroring across the main diagonal. Width and height swap. */
  IMAGE_ORIENTATION_LEFT_TOP = 5,

  /** Corrected by rotating 90 degrees clockwise. Width and height swap. */
  IMAGE_ORIENTATION_RIGHT_TOP = 6,

  /** Corrected by mirroring across the antidiagonal. Width and height swap. */
  IMAGE_ORIENTATION_RIGHT_BOTTOM = 7,

  /** Corrected by rotating 90 degrees counterclockwise. Width and height swap. */
  IMAGE_ORIENTATION_LEFT_BOTTOM = 8,
};

/** One malloc-owned, 8-bit image in interleaved RGB order. */
struct Image {
  /**
   * Pixel bytes, `width_px * height_px * IMAGE_CHANNEL_COUNT` long with no row padding. Owned by
   * the image and released by `image_free`.
   */
  unsigned char* pixels;

  /** Width in pixels, from 1 through `IMAGE_DIMENSION_MAX`. */
  size_t width_px;

  /** Height in pixels, from 1 through `IMAGE_DIMENSION_MAX`. */
  size_t height_px;
};

/**
 * @brief Reads JPEG dimensions without decoding its pixels.
 *
 * This rejects more than `IMAGE_INPUT_LEN_MAX` bytes.
 *
 * @param data       JPEG bytes. Must not be `NULL`.
 * @param data_len   Number of bytes available at `data`. More than `IMAGE_INPUT_LEN_MAX` is
 *                   rejected.
 * @param width_out  Receives the width on success. Must not be `NULL`.
 * @param height_out Receives the height on success. Must not be `NULL`.
 * @param reason     Destination for a failure diagnostic. May be `NULL` only when `reason_len` is
 *                   0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` for invalid JPEG data or dimensions.
 */
int image_probe(const unsigned char* data,
                size_t data_len,
                size_t* width_out,
                size_t* height_out,
                char* reason,
                size_t reason_len) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Decodes JPEG bytes into a malloc-owned RGB image.
 *
 * stb decodes at full resolution. `width_hint` and `height_hint` describe the largest useful output
 * and preserve the boundary needed by a future scaled decoder. Like `image_probe`, this rejects
 * more than `IMAGE_INPUT_LEN_MAX` input bytes.
 *
 * @param data        JPEG bytes. Must not be `NULL`.
 * @param data_len    Number of bytes available at `data`. More than `IMAGE_INPUT_LEN_MAX` is
 *                    rejected.
 * @param width_hint  Largest useful output width in pixels. May be 0 when unknown.
 * @param height_hint Largest useful output height in pixels. May be 0 when unknown.
 * @param image_out   Receives the decoded image on success and is zeroed on failure. Must not be
 *                    `NULL`.
 * @param reason      Destination for a failure diagnostic. May be `NULL` only when `reason_len` is
 *                    0.
 * @param reason_len  Size of `reason` in bytes.
 * @return `0` on success, or `-1` for invalid JPEG data, dimensions, or allocation failure.
 */
int image_decode(const unsigned char* data,
                 size_t data_len,
                 size_t width_hint,
                 size_t height_hint,
                 struct Image* image_out,
                 char* reason,
                 size_t reason_len) __attribute__((nonnull(1, 5)));

/**
 * @brief Applies an EXIF orientation to an image in place.
 *
 * `IMAGE_ORIENTATION_TOP_LEFT` leaves `image` untouched, so an upright source costs no second
 * full-size buffer. Any other orientation writes the transformed pixels into a new buffer, releases
 * the old one, and stores the new buffer and dimensions in `image`. The caller owns `image`
 * throughout and releases it with `image_free` either way.
 *
 * @param image       Malloc-owned RGB image to orient. Must contain valid bounded dimensions and
 *                    pixels. Left unchanged on failure. Must not be `NULL`.
 * @param orientation Orientation to apply.
 * @param reason      Destination for a failure diagnostic. May be `NULL` only when `reason_len` is
 *                    0.
 * @param reason_len  Size of `reason` in bytes.
 * @return `0` on success, or `-1` for invalid input or allocation failure.
 */
int image_orient(struct Image* image,
                 enum ImageOrientation orientation,
                 char* reason,
                 size_t reason_len) __attribute__((nonnull(1)));

/**
 * @brief Resizes an RGB image in linear light and returns a new malloc-owned image.
 *
 * @param image      Source RGB image. Must contain valid bounded dimensions and pixels.
 * @param width_px   Output width in pixels, from 1 through `IMAGE_DIMENSION_MAX`.
 * @param height_px  Output height in pixels, from 1 through `IMAGE_DIMENSION_MAX`.
 * @param image_out  Receives the resized image on success and is zeroed on failure. Must not be
 *                   `NULL`.
 * @param reason     Destination for a failure diagnostic. May be `NULL` only when `reason_len` is
 *                   0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` for invalid dimensions or allocation failure.
 */
int image_resize(const struct Image* image,
                 size_t width_px,
                 size_t height_px,
                 struct Image* image_out,
                 char* reason,
                 size_t reason_len) __attribute__((nonnull(1, 4)));

/**
 * @brief Resizes one rectangular view of an RGB image without allocating a crop buffer.
 *
 * The region is addressed in source pixels. The resizer receives a pointer to its first pixel and
 * the full source row stride, so cropping is only selection; interpolation remains stb's job.
 *
 * @param image            Source RGB image. Must contain valid bounded dimensions and pixels.
 * @param source_x_px      Left edge of the source region.
 * @param source_y_px      Top edge of the source region.
 * @param source_width_px  Source-region width; must be positive and inside `image`.
 * @param source_height_px Source-region height; must be positive and inside `image`.
 * @param width_px         Output width in pixels.
 * @param height_px        Output height in pixels.
 * @param image_out        Receives the resized image on success and is zeroed on failure. Must not
 *                         be `NULL`.
 * @param reason           Destination for a failure diagnostic. May be `NULL` only when
 *                         `reason_len` is 0.
 * @param reason_len       Size of `reason` in bytes.
 * @return `0` on success, or `-1` for an invalid region, dimensions, or allocation failure.
 */
int image_resize_region(const struct Image* image,
                        size_t source_x_px,
                        size_t source_y_px,
                        size_t source_width_px,
                        size_t source_height_px,
                        size_t width_px,
                        size_t height_px,
                        struct Image* image_out,
                        char* reason,
                        size_t reason_len) __attribute__((nonnull(1, 8)));

/**
 * @brief Encodes an RGB image as a bounded, malloc-owned JPEG byte buffer.
 *
 * @param image        Source RGB image. Must contain valid bounded dimensions and pixels.
 * @param quality      JPEG quality from 1 through 100.
 * @param data_out     Receives the malloc-owned JPEG bytes on success. Must not be `NULL`.
 * @param data_len_out Receives the JPEG byte length on success. Must not be `NULL`.
 * @param reason       Destination for a failure diagnostic. May be `NULL` only when `reason_len` is
 *                     0.
 * @param reason_len   Size of `reason` in bytes.
 * @return `0` on success, or `-1` for invalid input, allocation failure, or excessive output.
 */
int image_encode_jpeg(const struct Image* image,
                      size_t quality,
                      unsigned char** data_out,
                      size_t* data_len_out,
                      char* reason,
                      size_t reason_len) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Fits dimensions within a box without enlarging or changing aspect ratio.
 *
 * All input dimensions must be from 1 through `IMAGE_DIMENSION_MAX`.
 *
 * @param width_px      Source width in pixels.
 * @param height_px     Source height in pixels.
 * @param width_px_max  Box width in pixels.
 * @param height_px_max Box height in pixels.
 * @param width_out     Receives the fitted width. Must not be `NULL`.
 * @param height_out    Receives the fitted height. Must not be `NULL`.
 */
void image_fit(size_t width_px,
               size_t height_px,
               size_t width_px_max,
               size_t height_px_max,
               size_t* width_out,
               size_t* height_out) __attribute__((nonnull(5, 6)));

/**
 * @brief Selects the largest centered source rectangle matching a target aspect ratio.
 *
 * When an odd number of pixels must be discarded, the extra pixel remains on the right or bottom.
 * The function performs no allocation and cannot fail for positive bounded dimensions.
 *
 * @param width_px         Source width.
 * @param height_px        Source height.
 * @param target_width_px  Target aspect-ratio width.
 * @param target_height_px Target aspect-ratio height.
 * @param x_out            Receives the crop's left edge. Must not be `NULL`.
 * @param y_out            Receives the crop's top edge. Must not be `NULL`.
 * @param width_out        Receives the crop width. Must not be `NULL`.
 * @param height_out       Receives the crop height. Must not be `NULL`.
 */
void image_center_crop(size_t width_px,
                       size_t height_px,
                       size_t target_width_px,
                       size_t target_height_px,
                       size_t* x_out,
                       size_t* y_out,
                       size_t* width_out,
                       size_t* height_out) __attribute__((nonnull(5, 6, 7, 8)));

/**
 * @brief Returns the dimensions produced by an EXIF orientation.
 *
 * @param orientation Orientation to inspect.
 * @param width_px    Stored width in pixels.
 * @param height_px   Stored height in pixels.
 * @param width_out   Receives the presented width. Must not be `NULL`.
 * @param height_out  Receives the presented height. Must not be `NULL`.
 */
void image_orient_size(enum ImageOrientation orientation,
                       size_t width_px,
                       size_t height_px,
                       size_t* width_out,
                       size_t* height_out) __attribute__((nonnull(4, 5)));

/**
 * @brief Releases an image and resets it to the zero state.
 *
 * @param image Image to release. A zero image is accepted. Must not be `NULL`.
 */
void image_free(struct Image* image) __attribute__((nonnull(1)));

#endif
