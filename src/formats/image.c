#include "formats/image.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

/** Describes a destination-to-source coordinate transform. */
struct OrientationTransform {
  /**
   * Whether a destination pixel reads its source with the two axes swapped, which also swaps the
   * output's width and height.
   */
  bool is_transposed;

  /** Whether the source column is taken from the opposite edge, after any transpose. */
  bool is_mirrored_x;

  /** Whether the source row is taken from the opposite edge, after any transpose. */
  bool is_mirrored_y;
};

/** Accumulates the chunks emitted by stb's callback JPEG encoder. */
struct JpegOutput {
  /** Encoded bytes, grown with `realloc` and handed to the caller on success. */
  unsigned char* data;

  /** Bytes written so far, never above `IMAGE_ENCODED_LEN_MAX`. */
  size_t len;

  /** Bytes allocated at `data`. It doubles until it reaches `IMAGE_ENCODED_LEN_MAX`. */
  size_t capacity;

  /** Latched when growing `data` failed. Later chunks are then dropped instead of appended. */
  bool has_allocation_failed;

  /**
   * Latched when a chunk would push `len` past `IMAGE_ENCODED_LEN_MAX`. Later chunks are dropped.
   */
  bool has_exceeded_limit;
};

static const struct OrientationTransform orientation_transforms[] = {
    [IMAGE_ORIENTATION_TOP_LEFT] = {false, false, false},
    [IMAGE_ORIENTATION_TOP_RIGHT] = {false, true, false},
    [IMAGE_ORIENTATION_BOTTOM_RIGHT] = {false, true, true},
    [IMAGE_ORIENTATION_BOTTOM_LEFT] = {false, false, true},
    [IMAGE_ORIENTATION_LEFT_TOP] = {true, false, false},
    [IMAGE_ORIENTATION_RIGHT_TOP] = {true, false, true},
    [IMAGE_ORIENTATION_RIGHT_BOTTOM] = {true, true, true},
    [IMAGE_ORIENTATION_LEFT_BOTTOM] = {true, true, false},
};

_Static_assert((size_t)IMAGE_DIMENSION_MAX <= SIZE_MAX / (size_t)IMAGE_DIMENSION_MAX,
               "image dimension square must fit in size_t");
_Static_assert(IMAGE_PIXEL_COUNT_MAX <= SIZE_MAX / IMAGE_CHANNEL_COUNT,
               "image byte bound must fit in size_t");

/**
 * @brief Checks dimensions against the image limits.
 *
 * @param width_px   Width to check.
 * @param height_px  Height to check.
 * @param reason     Destination for a failure diagnostic.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` when the dimensions are valid, or `-1` otherwise.
 */
static int validate_dimensions(size_t width_px, size_t height_px, char* reason, size_t reason_len);

/**
 * @brief Checks an image's dimensions and pixel storage.
 *
 * @param image      Image to check. Must not be `NULL`.
 * @param reason     Destination for a failure diagnostic.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` when the image is valid, or `-1` otherwise.
 */
static int validate_image(const struct Image* image, char* reason, size_t reason_len)
    __attribute__((nonnull(1)));

/**
 * @brief Returns whether an orientation is one of the eight EXIF values.
 *
 * @param orientation Orientation to check.
 * @return `true` for a defined orientation, or `false` otherwise.
 */
static bool is_orientation_valid(enum ImageOrientation orientation);

/**
 * @brief Appends one encoder chunk to a bounded JPEG output.
 *
 * stb's callback cannot signal failure. This function latches a failure and ignores later chunks,
 * bounding retained memory even though stb finishes its current encode.
 *
 * @param context `struct JpegOutput` receiving the chunk. Must not be `NULL`.
 * @param data    Chunk bytes. Must not be `NULL` when `size` is positive.
 * @param size    Chunk length from stb.
 */
static void jpeg_write(void* context, void* data, int size) __attribute__((nonnull(1)));

int image_probe(const unsigned char* data,
                size_t data_len,
                size_t* width_out,
                size_t* height_out,
                char* reason,
                size_t reason_len) {
  if (data_len > IMAGE_INPUT_LEN_MAX) {
    return error_report(reason, reason_len,
                        "JPEG input exceeds max JPEG input length (%d bytes) at %zu bytes",
                        IMAGE_INPUT_LEN_MAX, data_len);
  }

  // stb's JPEG header parser rejects a zero width or height, and both are 16-bit fields, so a
  // successful probe always reports positive dimensions.
  int width = 0;
  int height = 0;
  if (stbi_info_from_memory(data, (int)data_len, &width, &height, NULL) == 0) {
    const char* failure = stbi_failure_reason();
    return error_report(reason, reason_len, "failed to probe JPEG: %s",
                        failure == NULL ? "unknown stb_image error" : failure);
  }
  if (validate_dimensions((size_t)width, (size_t)height, reason, reason_len) != 0) {
    return -1;
  }

  *width_out = (size_t)width;
  *height_out = (size_t)height;
  return 0;
}

int image_decode(const unsigned char* data,
                 size_t data_len,
                 size_t width_hint,
                 size_t height_hint,
                 struct Image* image_out,
                 char* reason,
                 size_t reason_len) {
  *image_out = (struct Image){0};

  // Reject hostile dimensions before stb allocates the full pixel buffer. Validating only the
  // dimensions returned by `stbi_load_from_memory` would enforce the model after its peak-memory
  // bound had already been lost. The probe also applies the input length limit. The decode below
  // parses the same header, so it reports the dimensions the probe validated.
  size_t width_probed = 0;
  size_t height_probed = 0;
  if (image_probe(data, data_len, &width_probed, &height_probed, reason, reason_len) != 0) {
    return -1;
  }

  // stb always decodes the full JPEG. Keeping these hints at the boundary allows a future scaled
  // decoder to reduce peak memory without changing its callers.
  (void)width_hint;
  (void)height_hint;

  int width = 0;
  int height = 0;
  unsigned char* pixels =
      stbi_load_from_memory(data, (int)data_len, &width, &height, NULL, IMAGE_CHANNEL_COUNT);
  if (pixels == NULL) {
    const char* failure = stbi_failure_reason();
    return error_report(reason, reason_len, "failed to decode JPEG: %s",
                        failure == NULL ? "unknown stb_image error" : failure);
  }

  image_out->pixels = pixels;
  image_out->width_px = (size_t)width;
  image_out->height_px = (size_t)height;
  return 0;
}

int image_orient(struct Image* image,
                 enum ImageOrientation orientation,
                 char* reason,
                 size_t reason_len) {
  if (validate_image(image, reason, reason_len) != 0) {
    return -1;
  }
  if (!is_orientation_valid(orientation)) {
    return error_report(reason, reason_len, "invalid image orientation: %d", (int)orientation);
  }
  if (orientation == IMAGE_ORIENTATION_TOP_LEFT) {
    return 0;
  }

  size_t width_out = 0;
  size_t height_out = 0;
  image_orient_size(orientation, image->width_px, image->height_px, &width_out, &height_out);
  unsigned char* pixels = malloc(width_out * height_out * IMAGE_CHANNEL_COUNT);
  if (pixels == NULL) {
    return error_report(reason, reason_len, "out of memory orienting image");
  }

  const struct OrientationTransform transform = orientation_transforms[orientation];
  for (size_t y = 0; y < height_out; y++) {
    for (size_t x = 0; x < width_out; x++) {
      size_t source_x = transform.is_transposed ? y : x;
      size_t source_y = transform.is_transposed ? x : y;
      if (transform.is_mirrored_x) {
        source_x = image->width_px - 1 - source_x;
      }
      if (transform.is_mirrored_y) {
        source_y = image->height_px - 1 - source_y;
      }
      const size_t source_offset = (source_y * image->width_px + source_x) * IMAGE_CHANNEL_COUNT;
      const size_t output_offset = (y * width_out + x) * IMAGE_CHANNEL_COUNT;
      memcpy(pixels + output_offset, image->pixels + source_offset, IMAGE_CHANNEL_COUNT);
    }
  }

  free(image->pixels);
  image->pixels = pixels;
  image->width_px = width_out;
  image->height_px = height_out;
  return 0;
}

int image_resize(const struct Image* image,
                 size_t width_px,
                 size_t height_px,
                 struct Image* image_out,
                 char* reason,
                 size_t reason_len) {
  return image_resize_region(image, 0, 0, image->width_px, image->height_px, width_px, height_px,
                             image_out, reason, reason_len);
}

int image_resize_region(const struct Image* image,
                        size_t source_x_px,
                        size_t source_y_px,
                        size_t source_width_px,
                        size_t source_height_px,
                        size_t width_px,
                        size_t height_px,
                        struct Image* image_out,
                        char* reason,
                        size_t reason_len) {
  *image_out = (struct Image){0};
  if (validate_image(image, reason, reason_len) != 0 ||
      validate_dimensions(source_width_px, source_height_px, reason, reason_len) != 0 ||
      validate_dimensions(width_px, height_px, reason, reason_len) != 0) {
    return -1;
  }
  if (source_x_px > image->width_px || source_width_px > image->width_px - source_x_px ||
      source_y_px > image->height_px || source_height_px > image->height_px - source_y_px) {
    return error_report(reason, reason_len,
                        "image crop is outside source dimensions: %zu,%zu %zu x %zu in %zu x %zu",
                        source_x_px, source_y_px, source_width_px, source_height_px,
                        image->width_px, image->height_px);
  }

  const size_t row_len = width_px * IMAGE_CHANNEL_COUNT;
  const size_t output_stride = (row_len + 3) / 4 * 4;
  const size_t data_len = row_len * height_px;
  const size_t allocation_len = output_stride * height_px;
  if (data_len == 0) {
    return error_report(reason, reason_len, "resized image has no pixels");
  }
  unsigned char* pixels = malloc(allocation_len);
  if (pixels == NULL) {
    return error_report(reason, reason_len, "out of memory resizing image");
  }

  const size_t source_offset = (source_y_px * image->width_px + source_x_px) * IMAGE_CHANNEL_COUNT;
  const size_t source_stride = image->width_px * IMAGE_CHANNEL_COUNT;
  if (stbir_resize_uint8_srgb(image->pixels + source_offset, (int)source_width_px,
                              (int)source_height_px, (int)source_stride, pixels, (int)width_px,
                              (int)height_px, (int)output_stride, STBIR_RGB) == NULL) {
    free(pixels);
    return error_report(reason, reason_len, "failed to resize image");
  }
  // stb's fast 8-bit encoder uses aligned integer stores. Padding every output row to four bytes
  // keeps those stores aligned for RGB widths whose natural stride is not divisible by four. Move
  // rows forward into the public tightly packed representation after the resize.
  if (output_stride != row_len) {
    for (size_t y = 1; y < height_px; y++) {
      memmove(pixels + y * row_len, pixels + y * output_stride, row_len);
    }
  }

  image_out->pixels = pixels;
  image_out->width_px = width_px;
  image_out->height_px = height_px;
  return 0;
}

int image_encode_jpeg(const struct Image* image,
                      size_t quality,
                      unsigned char** data_out,
                      size_t* data_len_out,
                      char* reason,
                      size_t reason_len) {
  *data_out = NULL;
  *data_len_out = 0;
  if (validate_image(image, reason, reason_len) != 0) {
    return -1;
  }
  if (quality < 1 || quality > 100) {
    return error_report(reason, reason_len, "JPEG quality must be from 1 through 100: %zu",
                        quality);
  }

  struct JpegOutput output = {0};
  const int is_encoded =
      stbi_write_jpg_to_func(jpeg_write, &output, (int)image->width_px, (int)image->height_px,
                             IMAGE_CHANNEL_COUNT, image->pixels, (int)quality);
  if (output.has_exceeded_limit) {
    free(output.data);
    return error_report(reason, reason_len, "JPEG output exceeds max encoded length (%d bytes)",
                        IMAGE_ENCODED_LEN_MAX);
  }
  if (output.has_allocation_failed) {
    free(output.data);
    return error_report(reason, reason_len, "out of memory encoding JPEG");
  }
  if (is_encoded == 0 || output.len == 0) {
    free(output.data);
    return error_report(reason, reason_len, "failed to encode JPEG");
  }

  *data_out = output.data;
  *data_len_out = output.len;
  return 0;
}

void image_fit(size_t width_px,
               size_t height_px,
               size_t width_px_max,
               size_t height_px_max,
               size_t* width_out,
               size_t* height_out) {
  if (width_px <= width_px_max && height_px <= height_px_max) {
    *width_out = width_px;
    *height_out = height_px;
    return;
  }

  if (width_px * height_px_max >= height_px * width_px_max) {
    *width_out = width_px_max;
    *height_out = height_px * width_px_max / width_px;
    if (*height_out == 0) {
      *height_out = 1;
    }
  } else {
    *width_out = width_px * height_px_max / height_px;
    *height_out = height_px_max;
    if (*width_out == 0) {
      *width_out = 1;
    }
  }
}

void image_center_crop(size_t width_px,
                       size_t height_px,
                       size_t target_width_px,
                       size_t target_height_px,
                       size_t* x_out,
                       size_t* y_out,
                       size_t* width_out,
                       size_t* height_out) {
  *x_out = 0;
  *y_out = 0;
  *width_out = width_px;
  *height_out = height_px;
  if (width_px * target_height_px > height_px * target_width_px) {
    *width_out = height_px * target_width_px / target_height_px;
    if (*width_out == 0) {
      *width_out = 1;
    }
    *x_out = (width_px - *width_out) / 2;
  } else if (width_px * target_height_px < height_px * target_width_px) {
    *height_out = width_px * target_height_px / target_width_px;
    if (*height_out == 0) {
      *height_out = 1;
    }
    *y_out = (height_px - *height_out) / 2;
  }
}

void image_orient_size(enum ImageOrientation orientation,
                       size_t width_px,
                       size_t height_px,
                       size_t* width_out,
                       size_t* height_out) {
  const bool is_transposed =
      is_orientation_valid(orientation) && orientation_transforms[orientation].is_transposed;
  *width_out = is_transposed ? height_px : width_px;
  *height_out = is_transposed ? width_px : height_px;
}

void image_free(struct Image* image) {
  free(image->pixels);
  *image = (struct Image){0};
}

static int validate_dimensions(size_t width_px, size_t height_px, char* reason, size_t reason_len) {
  if (width_px == 0 || height_px == 0) {
    return error_report(reason, reason_len, "image dimensions must be positive: %zu x %zu",
                        width_px, height_px);
  }
  if (width_px > IMAGE_DIMENSION_MAX || height_px > IMAGE_DIMENSION_MAX) {
    return error_report(reason, reason_len, "image exceeds max dimension (%d pixels): %zu x %zu",
                        IMAGE_DIMENSION_MAX, width_px, height_px);
  }
  if (width_px > IMAGE_PIXEL_COUNT_MAX / height_px) {
    return error_report(reason, reason_len, "image exceeds max pixel count (%d pixels): %zu x %zu",
                        IMAGE_PIXEL_COUNT_MAX, width_px, height_px);
  }
  return 0;
}

static int validate_image(const struct Image* image, char* reason, size_t reason_len) {
  if (image->pixels == NULL) {
    return error_report(reason, reason_len, "image pixels are missing");
  }
  return validate_dimensions(image->width_px, image->height_px, reason, reason_len);
}

static bool is_orientation_valid(enum ImageOrientation orientation) {
  return orientation >= IMAGE_ORIENTATION_TOP_LEFT && orientation <= IMAGE_ORIENTATION_LEFT_BOTTOM;
}

static void jpeg_write(void* context, void* data, int size) {
  struct JpegOutput* output = context;
  if (output->has_allocation_failed || output->has_exceeded_limit || size <= 0) {
    return;
  }
  const size_t chunk_len = (size_t)size;
  if (chunk_len > IMAGE_ENCODED_LEN_MAX - output->len) {
    output->has_exceeded_limit = true;
    return;
  }

  const size_t len_next = output->len + chunk_len;
  if (len_next > output->capacity) {
    size_t capacity_next = output->capacity == 0 ? 4096 : output->capacity;
    while (capacity_next < len_next) {
      if (capacity_next > IMAGE_ENCODED_LEN_MAX / 2) {
        capacity_next = IMAGE_ENCODED_LEN_MAX;
      } else {
        capacity_next *= 2;
      }
    }
    unsigned char* data_next = realloc(output->data, capacity_next);
    if (data_next == NULL) {
      output->has_allocation_failed = true;
      return;
    }
    output->data = data_next;
    output->capacity = capacity_next;
  }

  memcpy(output->data + output->len, data, chunk_len);
  output->len = len_next;
}
