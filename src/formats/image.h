#ifndef FRAM_IMAGE_H
#define FRAM_IMAGE_H

#include <limits.h>
#include <stddef.h>

#include "shared/str.h"

struct Arena;
struct Log;

/** Number of interleaved color channels in every decoded image. */
enum { IMAGE_CHANNEL_COUNT = 3 };

/** Largest accepted width or height, in pixels. */
enum { IMAGE_DIMENSION_MAX = 20000 };

/** Largest accepted width-times-height product, in pixels. */
enum { IMAGE_PIXEL_COUNT_MAX = 100000000 };

/**
 * Longest JPEG a caller reads, in bytes, far longer than any JPEG within `IMAGE_PIXEL_COUNT_MAX`
 * needs. A caller passes this bound to `os_read`, so an oversize file is rejected from its size
 * before it is loaded, and a worker's scratch holds one source of this length.
 */
enum { IMAGE_INPUT_LEN_MAX = INT_MAX };

/**
 * Scale of a full-size decode, in eighths. `image_decode` scales each dimension by
 * `scale_eighths / IMAGE_SCALE_FULL`, for `scale_eighths` from 1 through `IMAGE_SCALE_FULL`.
 */
enum { IMAGE_SCALE_FULL = 8 };

/** Largest encoded width or height, in pixels, the most a WebP image's 14-bit size fields hold. */
enum { IMAGE_ENCODED_DIMENSION_MAX = 16383 };

/**
 * Bytes, for each pixel of an encoded image, that resizing to it and encoding it hold: the resized
 * pixels' three and up to 32 for libwebp's encoder, which measured up to 9.4 for photographs at
 * quality 90 and 29 for noise at quality 100.
 */
enum { IMAGE_ENCODE_LEN_PER_PX = IMAGE_CHANNEL_COUNT + 32 };

/**
 * Bytes a worker's scratch keeps for one encoded WebP. The largest measured, a 10000 x 10000 image
 * of noise at quality 100, took 110 MB, and an encode past this could run the scratch out, which
 * ends the build in `os_oom`.
 */
enum { IMAGE_ENCODED_LEN_MAX = 512 * 1024 * 1024 };

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

/** What a JPEG's header says about the image and the memory a decode of it holds. */
struct ImageHeader {
  /** Width in pixels, from 1 through `IMAGE_DIMENSION_MAX`. */
  ptrdiff_t width_px;

  /** Height in pixels, from 1 through `IMAGE_DIMENSION_MAX`. */
  ptrdiff_t height_px;

  /**
   * Bytes of DCT coefficients a decode holds at any scale, beside its pixels. A progressive or
   * other multi-scan JPEG keeps a coefficient for every block of the full-size image for the whole
   * decode, two bytes for each of a component's samples. A single-scan JPEG decodes row by row and
   * holds none, so this is `0`.
   */
  ptrdiff_t coefficient_len;
};

/**
 * One malloc-owned, 8-bit image in interleaved RGB order.
 *
 * Pixels stay on `malloc` rather than an arena. They are large, transient, allocated once per file
 * and bounded by the derivative phase's decode budget, and an arena would keep each worker's
 * largest decode committed for the rest of the build. A caller releases an image with `image_free`.
 */
struct Image {
  /**
   * Pixel bytes, `width_px * height_px * IMAGE_CHANNEL_COUNT` long with no row padding. Owned by
   * the image and released by `image_free`.
   */
  unsigned char* pixels;

  /** Width in pixels, from 1 through `IMAGE_DIMENSION_MAX`. */
  ptrdiff_t width_px;

  /** Height in pixels, from 1 through `IMAGE_DIMENSION_MAX`. */
  ptrdiff_t height_px;
};

/**
 * @brief Reads a JPEG's header without decoding its pixels.
 *
 * This rejects a JPEG that `image_decode` cannot decode for its header alone: a sample precision
 * other than 8 bits, a lossless JPEG, which libjpeg-turbo decodes only at full size, or a color
 * space other than grayscale, YCbCr, RGB, CMYK, or YCCK. A 1 x 1 lossless RGB JPEG is the
 * exception: no scale changes its size, so it probes and decodes.
 *
 * @param data       JPEG bytes.
 * @param header_out Receives the header on success. Must not be `NULL`.
 * @param operation  Operation leading the failure message.
 * @param file_path  File the bytes were read from or go to, named at the end of the message.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` for invalid or unsupported JPEG data or dimensions.
 */
int image_probe(struct Str data,
                struct ImageHeader* header_out,
                struct Str operation,
                struct Str file_path,
                struct Log* log) __attribute__((nonnull(2, 5)));

/**
 * @brief Decodes JPEG bytes into a malloc-owned RGB image, scaled down in the DCT.
 *
 * libjpeg-turbo scales while it decodes, so a reduced scale decodes fewer pixels rather than
 * shrinking a full-size image, and the image has the dimensions `image_scale_size` gives. A CMYK or
 * YCCK JPEG is converted to RGB. Like `image_probe`, this rejects an unsupported header, and it
 * rejects a JPEG of more than 100 scans, which only a hostile file has. A corrupt scan decodes as
 * far as the data allows rather than failing, and so do damaged bytes after the last row.
 *
 * @param data          JPEG bytes.
 * @param scale_eighths Scale of each dimension, in eighths, from 1 through `IMAGE_SCALE_FULL`.
 * @param image_out     Receives the decoded image on success and is zeroed on failure. Must not be
 *                      `NULL`.
 * @param operation     Operation leading the failure message.
 * @param file_path     File the bytes were read from or go to, named at the end of the message.
 * @param log           Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` for invalid or unsupported JPEG data or dimensions.
 */
int image_decode(struct Str data,
                 ptrdiff_t scale_eighths,
                 struct Image* image_out,
                 struct Str operation,
                 struct Str file_path,
                 struct Log* log) __attribute__((nonnull(3, 6)));

/**
 * @brief Returns the most bytes that decoding a JPEG and then orienting it hold at once.
 *
 * The decode holds its scaled pixels beside a multi-scan JPEG's coefficients, which it releases
 * before it returns. A rotated or mirrored orientation then holds two copies of the pixels while it
 * runs. The result is the larger of the two.
 *
 * @param header        Header `image_probe` read. Must not be `NULL`.
 * @param scale_eighths Decode scale, from 1 through `IMAGE_SCALE_FULL`.
 * @param orientation   Orientation `image_orient` is given afterwards.
 * @return Bytes the decode and orientation hold at their peak.
 */
ptrdiff_t image_decode_len(const struct ImageHeader* header,
                           ptrdiff_t scale_eighths,
                           enum ImageOrientation orientation) __attribute__((nonnull(1)));

/**
 * @brief Returns the dimensions `image_decode` produces at a scale.
 *
 * Each dimension is rounded up, so none scales to zero.
 *
 * @param width_px      Full-size width in pixels, from 1 through `IMAGE_DIMENSION_MAX`.
 * @param height_px     Full-size height in pixels, from 1 through `IMAGE_DIMENSION_MAX`.
 * @param scale_eighths Scale of each dimension, in eighths, from 1 through `IMAGE_SCALE_FULL`.
 * @param width_out     Receives the scaled width. Must not be `NULL`.
 * @param height_out    Receives the scaled height. Must not be `NULL`.
 */
void image_scale_size(ptrdiff_t width_px,
                      ptrdiff_t height_px,
                      ptrdiff_t scale_eighths,
                      ptrdiff_t* width_out,
                      ptrdiff_t* height_out) __attribute__((nonnull(4, 5)));

/**
 * @brief Applies an EXIF orientation to an image in place.
 *
 * `IMAGE_ORIENTATION_TOP_LEFT` leaves `image` untouched, so an upright source costs no second
 * full-size buffer. Any other orientation writes the transformed pixels into a new buffer, releases
 * the old one, and stores the new buffer and dimensions in `image`. The caller owns `image`
 * throughout and releases it with `image_free` either way.
 *
 * @param image       Malloc-owned RGB image to orient, with valid bounded dimensions and pixels.
 *                    Must not be `NULL`.
 * @param orientation One of the eight EXIF orientations.
 */
void image_orient(struct Image* image, enum ImageOrientation orientation)
    __attribute__((nonnull(1)));

/**
 * @brief Resizes one rectangular view of an RGB image without allocating a crop buffer.
 *
 * The region is addressed in source pixels. The resizer receives a pointer to its first pixel and
 * the full source row stride, so cropping is only selection; interpolation remains stb's job.
 *
 * @param image            Source RGB image. Must contain valid bounded dimensions and pixels.
 * @param source_x_px      Left edge of the source region, from 0.
 * @param source_y_px      Top edge of the source region, from 0.
 * @param source_width_px  Source-region width; must be positive and inside `image`.
 * @param source_height_px Source-region height; must be positive and inside `image`.
 * @param width_px         Output width in pixels, from 1 through `IMAGE_DIMENSION_MAX`, with
 *                         `width_px * height_px` at most `IMAGE_PIXEL_COUNT_MAX`.
 * @param height_px        Output height in pixels, from 1 through `IMAGE_DIMENSION_MAX`.
 * @param image_out        Receives the resized image on success and is zeroed on failure. Must not
 *                         be `NULL`.
 * @param operation        Operation leading the failure message.
 * @param file_path        File the bytes were read from or go to, named at the end of the message.
 * @param log              Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the resizer fails.
 */
int image_resize_region(const struct Image* image,
                        ptrdiff_t source_x_px,
                        ptrdiff_t source_y_px,
                        ptrdiff_t source_width_px,
                        ptrdiff_t source_height_px,
                        ptrdiff_t width_px,
                        ptrdiff_t height_px,
                        struct Image* image_out,
                        struct Str operation,
                        struct Str file_path,
                        struct Log* log) __attribute__((nonnull(1, 8, 11)));

/**
 * @brief Encodes an RGB image as a lossy WebP held in `perm`.
 *
 * The bytes grow as one buffer at the end of `perm`, so the encoder copies nothing as they grow. A
 * failure leaves `perm` as it found it. The encoder's own working memory is on `malloc` and is
 * released before this returns, and a failed allocation inside it ends the build in `os_oom`.
 *
 * @param image     Source RGB image. Must contain valid pixels, and dimensions from 1 through
 *                  `IMAGE_ENCODED_DIMENSION_MAX`.
 * @param quality   WebP quality from 1 through 100.
 * @param perm      Arena the encoded bytes are allocated from. Must not be `NULL`.
 * @param webp_out  Receives the encoded bytes on success. Written only on success. Must not be
 *                  `NULL`.
 * @param operation Operation leading the failure message.
 * @param file_path File the bytes go to, named at the end of the message.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` for an encoder failure.
 */
int image_encode_webp(const struct Image* image,
                      ptrdiff_t quality,
                      struct Arena* perm,
                      struct Str* webp_out,
                      struct Str operation,
                      struct Str file_path,
                      struct Log* log) __attribute__((nonnull(1, 3, 4, 7)));

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
void image_fit(ptrdiff_t width_px,
               ptrdiff_t height_px,
               ptrdiff_t width_px_max,
               ptrdiff_t height_px_max,
               ptrdiff_t* width_out,
               ptrdiff_t* height_out) __attribute__((nonnull(5, 6)));

/**
 * @brief Selects the largest centered source rectangle matching a target aspect ratio.
 *
 * When an odd number of pixels must be discarded, the extra pixel remains on the right or bottom.
 * The function performs no allocation and cannot fail for positive bounded dimensions.
 *
 * @param width_px         Source width.
 * @param height_px        Source height.
 * @param width_px_target  Target aspect-ratio width.
 * @param height_px_target Target aspect-ratio height.
 * @param x_out            Receives the crop's left edge. Must not be `NULL`.
 * @param y_out            Receives the crop's top edge. Must not be `NULL`.
 * @param width_out        Receives the crop width. Must not be `NULL`.
 * @param height_out       Receives the crop height. Must not be `NULL`.
 */
void image_center_crop(ptrdiff_t width_px,
                       ptrdiff_t height_px,
                       ptrdiff_t width_px_target,
                       ptrdiff_t height_px_target,
                       ptrdiff_t* x_out,
                       ptrdiff_t* y_out,
                       ptrdiff_t* width_out,
                       ptrdiff_t* height_out) __attribute__((nonnull(5, 6, 7, 8)));

/**
 * @brief Returns the dimensions produced by an EXIF orientation.
 *
 * @param orientation One of the eight EXIF orientations.
 * @param width_px    Stored width in pixels.
 * @param height_px   Stored height in pixels.
 * @param width_out   Receives the presented width. Must not be `NULL`.
 * @param height_out  Receives the presented height. Must not be `NULL`.
 */
void image_orient_size(enum ImageOrientation orientation,
                       ptrdiff_t width_px,
                       ptrdiff_t height_px,
                       ptrdiff_t* width_out,
                       ptrdiff_t* height_out) __attribute__((nonnull(4, 5)));

/**
 * @brief Allocates `size` bytes like `malloc`, calling `os_oom` instead of returning a failure.
 *
 * This is the allocator behind every pixel buffer this module allocates, and behind the working
 * memory of stb's resizer, which `vendor/stb/stb.c` routes here through stb's allocator hooks. A
 * failed pixel allocation therefore ends the build like any other failed allocation, and the report
 * names the file, rather than reaching a caller as a decode error. Release the memory with `free`.
 *
 * @param size Bytes to allocate. A request above `PTRDIFF_MAX` cannot succeed and calls `os_oom`.
 * @return The allocation. `NULL` only for a zero `size`, as `malloc` allows.
 */
void* image_alloc(size_t size);

/**
 * @brief Releases an image and resets it to the zero state.
 *
 * @param image Image to release. A zero image is accepted. Must not be `NULL`.
 */
void image_free(struct Image* image) __attribute__((nonnull(1)));

#endif
