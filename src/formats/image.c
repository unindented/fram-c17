#include "formats/image.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
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

/** Collects the chunks stb's callback JPEG encoder emits into one bounded buffer. */
struct JpegOutput {
  /** Bytes taken so far, never more than `IMAGE_ENCODED_LEN_MAX`. */
  struct Buf buf;

  /**
   * Bytes stb emitted so far. Once they pass `IMAGE_ENCODED_LEN_MAX`, later chunks are only
   * counted, so the error can name the whole encoded length.
   */
  ptrdiff_t len;

  /**
   * Latched when a chunk would push `len` past `IMAGE_ENCODED_LEN_MAX`. That chunk and every later
   * one are dropped.
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

_Static_assert((ptrdiff_t)IMAGE_DIMENSION_MAX <= PTRDIFF_MAX / (ptrdiff_t)IMAGE_DIMENSION_MAX,
               "image dimension square must fit in ptrdiff_t");
_Static_assert(IMAGE_PIXEL_COUNT_MAX <= PTRDIFF_MAX / IMAGE_CHANNEL_COUNT,
               "image byte bound must fit in ptrdiff_t");

/**
 * Where an operation reports its failure: the caller's operation, which leads the message, the file
 * it trails with, and the log, as in
 * `failed to probe media: failed to probe JPEG: Corrupt JPEG ('photos/a.jpg')`.
 */
struct ImageReport {
  /** Operation the caller was performing, such as `failed to probe media`. */
  struct Str operation;

  /** File the bytes were read from or are written to. */
  struct Str file_path;

  /** Log that receives the message. */
  struct Log* log;
};

/**
 * @brief Checks dimensions against the image limits.
 *
 * @param width_px  Width to check.
 * @param height_px Height to check.
 * @param report    Where a failure is reported. Must not be `NULL`.
 * @return `0` when the dimensions are valid, or `-1` otherwise.
 */
static int validate_dimensions(ptrdiff_t width_px,
                               ptrdiff_t height_px,
                               const struct ImageReport* report) __attribute__((nonnull(3)));

/**
 * @brief Appends one encoder chunk to a bounded JPEG output.
 *
 * stb's callback cannot signal failure. This function latches an output that would exceed
 * `IMAGE_ENCODED_LEN_MAX` and drops later chunks, bounding the buffer even though stb finishes its
 * current encode. Past the limit it still counts each chunk, so the error names the whole encoded
 * length.
 *
 * @param context `struct JpegOutput` receiving the chunk. Must not be `NULL`.
 * @param data    Chunk bytes. Must not be `NULL` when `size` is positive.
 * @param size    Chunk length from stb.
 */
static void jpeg_write(void* context, void* data, int size) __attribute__((nonnull(1)));

int image_probe(struct Str data,
                ptrdiff_t* width_out,
                ptrdiff_t* height_out,
                struct Str operation,
                struct Str file_path,
                struct Log* log) {
  const struct ImageReport report = {operation, file_path, log};
  if (data.len > IMAGE_INPUT_LEN_MAX) {
    return log_error(
        log, "%.*s: JPEG input exceeds max JPEG input length (%d bytes) at %td bytes ('%.*s')",
        STR_FMT(operation), IMAGE_INPUT_LEN_MAX, data.len, STR_FMT(file_path));
  }

  // stb's JPEG header parser rejects a zero width or height, and both are 16-bit fields, so a
  // successful probe always reports positive dimensions.
  int width = 0;
  int height = 0;
  if (stbi_info_from_memory((const unsigned char*)data.data, (int)data.len, &width, &height,
                            NULL) == 0) {
    const char* failure = stbi_failure_reason();
    return log_error(log, "%.*s: failed to probe JPEG: %s ('%.*s')", STR_FMT(operation),
                     failure == NULL ? "unknown stb_image error" : failure, STR_FMT(file_path));
  }
  if (validate_dimensions(width, height, &report) != 0) {
    return -1;
  }

  *width_out = width;
  *height_out = height;
  return 0;
}

int image_decode(struct Str data,
                 struct Image* image_out,
                 struct Str operation,
                 struct Str file_path,
                 struct Log* log) {
  *image_out = (struct Image){0};

  // Reject hostile dimensions before stb allocates the full pixel buffer. Validating only the
  // dimensions returned by `stbi_load_from_memory` would enforce the model after its peak-memory
  // bound had already been lost. The probe also applies the input length limit. The decode below
  // parses the same header, so it reports the dimensions the probe validated.
  ptrdiff_t width_probed = 0;
  ptrdiff_t height_probed = 0;
  if (image_probe(data, &width_probed, &height_probed, operation, file_path, log) != 0) {
    return -1;
  }

  int width = 0;
  int height = 0;
  unsigned char* pixels = stbi_load_from_memory((const unsigned char*)data.data, (int)data.len,
                                                &width, &height, NULL, IMAGE_CHANNEL_COUNT);
  if (pixels == NULL) {
    const char* failure = stbi_failure_reason();
    return log_error(log, "%.*s: failed to decode JPEG: %s ('%.*s')", STR_FMT(operation),
                     failure == NULL ? "unknown stb_image error" : failure, STR_FMT(file_path));
  }

  image_out->pixels = pixels;
  image_out->width_px = width;
  image_out->height_px = height;
  return 0;
}

void image_orient(struct Image* image, enum ImageOrientation orientation) {
  if (orientation == IMAGE_ORIENTATION_TOP_LEFT) {
    return;
  }

  ptrdiff_t width_out = 0;
  ptrdiff_t height_out = 0;
  image_orient_size(orientation, image->width_px, image->height_px, &width_out, &height_out);
  unsigned char* pixels = image_alloc((size_t)(width_out * height_out * IMAGE_CHANNEL_COUNT));

  const struct OrientationTransform transform = orientation_transforms[orientation];
  for (ptrdiff_t y = 0; y < height_out; y++) {
    for (ptrdiff_t x = 0; x < width_out; x++) {
      ptrdiff_t source_x = transform.is_transposed ? y : x;
      ptrdiff_t source_y = transform.is_transposed ? x : y;
      if (transform.is_mirrored_x) {
        source_x = image->width_px - 1 - source_x;
      }
      if (transform.is_mirrored_y) {
        source_y = image->height_px - 1 - source_y;
      }
      const ptrdiff_t source_offset = (source_y * image->width_px + source_x) * IMAGE_CHANNEL_COUNT;
      const ptrdiff_t output_offset = (y * width_out + x) * IMAGE_CHANNEL_COUNT;
      memcpy(pixels + output_offset, image->pixels + source_offset, IMAGE_CHANNEL_COUNT);
    }
  }

  free(image->pixels);
  image->pixels = pixels;
  image->width_px = width_out;
  image->height_px = height_out;
}

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
                        struct Log* log) {
  *image_out = (struct Image){0};
  const ptrdiff_t row_len = width_px * IMAGE_CHANNEL_COUNT;
  const ptrdiff_t output_stride = (row_len + 3) / 4 * 4;
  const ptrdiff_t allocation_len = output_stride * height_px;
  unsigned char* pixels = image_alloc((size_t)allocation_len);

  const ptrdiff_t source_offset =
      (source_y_px * image->width_px + source_x_px) * IMAGE_CHANNEL_COUNT;
  const ptrdiff_t source_stride = image->width_px * IMAGE_CHANNEL_COUNT;
  if (stbir_resize_uint8_srgb(image->pixels + source_offset, (int)source_width_px,
                              (int)source_height_px, (int)source_stride, pixels, (int)width_px,
                              (int)height_px, (int)output_stride, STBIR_RGB) == NULL) {
    free(pixels);
    return log_error(log, "%.*s: failed to resize image ('%.*s')", STR_FMT(operation),
                     STR_FMT(file_path));
  }
  // stb's fast 8-bit encoder uses aligned integer stores. Padding every output row to four bytes
  // keeps those stores aligned for RGB widths whose natural stride is not divisible by four. Move
  // rows forward into the public tightly packed representation after the resize.
  if (output_stride != row_len) {
    for (ptrdiff_t y = 1; y < height_px; y++) {
      memmove(pixels + y * row_len, pixels + y * output_stride, (size_t)row_len);
    }
  }

  image_out->pixels = pixels;
  image_out->width_px = width_px;
  image_out->height_px = height_px;
  return 0;
}

int image_encode_jpeg(const struct Image* image,
                      ptrdiff_t quality,
                      struct Arena* perm,
                      struct Str* jpeg_out,
                      struct Str operation,
                      struct Str file_path,
                      struct Log* log) {
  // The bytes grow in a copy of `perm`, which is published only on success.
  struct Arena arena = *perm;
  struct JpegOutput output = {.buf = {.arena = &arena}};
  const int is_encoded =
      stbi_write_jpg_to_func(jpeg_write, &output, (int)image->width_px, (int)image->height_px,
                             IMAGE_CHANNEL_COUNT, image->pixels, (int)quality);
  if (output.has_exceeded_limit) {
    return log_error(log,
                     "%.*s: JPEG output exceeds max encoded length (%d bytes) at %td bytes "
                     "('%.*s')",
                     STR_FMT(operation), IMAGE_ENCODED_LEN_MAX, output.len, STR_FMT(file_path));
  }
  if (is_encoded == 0 || output.len == 0) {
    return log_error(log, "%.*s: failed to encode JPEG ('%.*s')", STR_FMT(operation),
                     STR_FMT(file_path));
  }
  *perm = arena;
  *jpeg_out = buf_str(&output.buf);
  return 0;
}

void image_fit(ptrdiff_t width_px,
               ptrdiff_t height_px,
               ptrdiff_t width_px_max,
               ptrdiff_t height_px_max,
               ptrdiff_t* width_out,
               ptrdiff_t* height_out) {
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

void image_center_crop(ptrdiff_t width_px,
                       ptrdiff_t height_px,
                       ptrdiff_t width_px_target,
                       ptrdiff_t height_px_target,
                       ptrdiff_t* x_out,
                       ptrdiff_t* y_out,
                       ptrdiff_t* width_out,
                       ptrdiff_t* height_out) {
  *x_out = 0;
  *y_out = 0;
  *width_out = width_px;
  *height_out = height_px;
  if (width_px * height_px_target > height_px * width_px_target) {
    *width_out = height_px * width_px_target / height_px_target;
    if (*width_out == 0) {
      *width_out = 1;
    }
    *x_out = (width_px - *width_out) / 2;
  } else if (width_px * height_px_target < height_px * width_px_target) {
    *height_out = width_px * height_px_target / width_px_target;
    if (*height_out == 0) {
      *height_out = 1;
    }
    *y_out = (height_px - *height_out) / 2;
  }
}

void image_orient_size(enum ImageOrientation orientation,
                       ptrdiff_t width_px,
                       ptrdiff_t height_px,
                       ptrdiff_t* width_out,
                       ptrdiff_t* height_out) {
  const bool is_transposed = orientation_transforms[orientation].is_transposed;
  *width_out = is_transposed ? height_px : width_px;
  *height_out = is_transposed ? width_px : height_px;
}

void* image_alloc(size_t size) {
  return image_realloc(NULL, size);
}

void* image_realloc(void* data, size_t size) {
  // A zero size frees, as `realloc` may, but explicitly, because what `realloc` returns for it is
  // implementation-defined.
  if (size == 0) {
    free(data);
    return NULL;
  }
  // No object can be larger than `PTRDIFF_MAX`, because a pointer difference inside it would
  // overflow, so a larger request cannot succeed and is refused before `realloc` sees it.
  void* result = size <= (size_t)PTRDIFF_MAX ? realloc(data, size) : NULL;
  if (result == NULL) {
    os_oom();
  }
  return result;
}

void image_free(struct Image* image) {
  free(image->pixels);
  *image = (struct Image){0};
}

static int validate_dimensions(ptrdiff_t width_px,
                               ptrdiff_t height_px,
                               const struct ImageReport* report) {
  if (width_px <= 0 || height_px <= 0) {
    return log_error(report->log, "%.*s: image dimensions must be positive: %td x %td ('%.*s')",
                     STR_FMT(report->operation), width_px, height_px, STR_FMT(report->file_path));
  }
  if (width_px > IMAGE_DIMENSION_MAX || height_px > IMAGE_DIMENSION_MAX) {
    return log_error(report->log,
                     "%.*s: image exceeds max dimension (%d pixels) at %td x %td ('%.*s')",
                     STR_FMT(report->operation), IMAGE_DIMENSION_MAX, width_px, height_px,
                     STR_FMT(report->file_path));
  }
  if (width_px > IMAGE_PIXEL_COUNT_MAX / height_px) {
    return log_error(report->log,
                     "%.*s: image exceeds max pixel count (%d pixels) at %td x %td ('%.*s')",
                     STR_FMT(report->operation), IMAGE_PIXEL_COUNT_MAX, width_px, height_px,
                     STR_FMT(report->file_path));
  }
  return 0;
}

static void jpeg_write(void* context, void* data, int size) {
  struct JpegOutput* output = context;
  if (size <= 0) {
    return;
  }
  if (output->has_exceeded_limit || size > IMAGE_ENCODED_LEN_MAX - output->len) {
    output->has_exceeded_limit = true;
    // The count saturates instead of wrapping when the output is too large for a `ptrdiff_t`.
    output->len = size > PTRDIFF_MAX - output->len ? PTRDIFF_MAX : output->len + size;
    return;
  }
  buf_append(&output->buf, (struct Str){data, size});
  output->len += size;
}
