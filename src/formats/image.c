#include "formats/image.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// `jpeglib.h` uses `size_t` and `FILE` without including their headers.
#include "jerror.h"
#include "jpeglib.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
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

/**
 * Most scans a JPEG may have. A progressive JPEG has about ten, and a file can repeat a scan to
 * make its decode take minutes: at this count, a 100-megapixel one still takes seconds.
 */
enum { JPEG_SCAN_COUNT_MAX = 100 };

/**
 * One run of the codec, which the library reaches through `client_data`. `decoder_read` calls
 * `setjmp`, so the state it changes lives in its caller's frame, where the jump back leaves its
 * value defined.
 */
struct JpegDecoder {
  /** The library's decompression state. */
  struct jpeg_decompress_struct decompress;

  /** The library's error manager. Its `error_exit` jumps to `exit` and does not return. */
  struct jpeg_error_mgr error;

  /** The library's progress hook, which stops a decode with too many scans. */
  struct jpeg_progress_mgr progress;

  /** Where a fatal error jumps to, set by `decoder_read` before it starts the codec. */
  jmp_buf exit;

  /** The fatal error's message. */
  char message[JMSG_LENGTH_MAX];
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
 * `failed to probe media: failed to probe JPEG: Not a JPEG file: starts with 0x6e 0x6f ('a.jpg')`.
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
 * libjpeg-turbo rejects a zero width or height while it reads the header, so both are positive.
 *
 * @param width_px  Width to check, positive.
 * @param height_px Height to check, positive.
 * @param report    Where a failure is reported. Must not be `NULL`.
 * @return `0` when the dimensions are valid, or `-1` otherwise.
 */
static int validate_dimensions(ptrdiff_t width_px,
                               ptrdiff_t height_px,
                               const struct ImageReport* report) __attribute__((nonnull(3)));

/**
 * @brief Reads a JPEG's header and, when `image_out` is not `NULL`, decodes its pixels.
 *
 * A fatal codec error jumps back here from inside the library, which leaves `decoder` and
 * `image_out` defined because both live in the caller's frame. The decoder's memory is released on
 * every path.
 *
 * @param decoder       Codec state, in the caller's frame. Must not be `NULL`.
 * @param data          JPEG bytes.
 * @param header_out    Receives the header once it is read. Must not be `NULL`.
 * @param scale_eighths Decode scale, from 1 through `IMAGE_SCALE_FULL`. Unused without `image_out`.
 * @param image_out     Receives the decoded image, or `NULL` to read the header only. It must be
 *                      zero on entry and is zeroed again on failure.
 * @param report        Where a failure is reported. Must not be `NULL`.
 * @return `0` on success, or `-1` for invalid or unsupported JPEG data or dimensions.
 */
static int decoder_read(struct JpegDecoder* decoder,
                        struct Str data,
                        struct ImageHeader* header_out,
                        ptrdiff_t scale_eighths,
                        struct Image* image_out,
                        const struct ImageReport* report) __attribute__((nonnull(1, 3, 6)));

/**
 * @brief Rejects a header `image_decode` cannot decode, and fills in what it says.
 *
 * Dimensions outside the image limits are reported here. Any other rejection is a fatal codec
 * error, so it jumps back to `decoder_read` and does not return.
 *
 * @param decoder    Codec state whose header has been read. Must not be `NULL`.
 * @param header_out Receives the header. Must not be `NULL`.
 * @param report     Where a dimension failure is reported. Must not be `NULL`.
 * @return `0` when the header can be decoded, or `-1` for dimensions outside the limits.
 */
static int decoder_read_header_check(struct JpegDecoder* decoder,
                                     struct ImageHeader* header_out,
                                     const struct ImageReport* report)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Decodes every row of a started decompression into a new image.
 *
 * @param decoder   Codec state after `jpeg_start_decompress`. Must not be `NULL`.
 * @param is_cmyk   Whether rows arrive as CMYK to be converted, rather than as RGB.
 * @param image_out Receives the pixels as they are allocated. Must not be `NULL`.
 */
static void decoder_read_rows(struct JpegDecoder* decoder, bool is_cmyk, struct Image* image_out)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Formats the library's message for a fatal error and jumps back to `decoder_read`.
 *
 * The library reports a failed allocation as a fatal error, and that one ends the build in
 * `os_oom` instead, like every other failed allocation.
 *
 * @param common The codec, whose `client_data` is its `struct JpegDecoder`. Must not be `NULL`.
 */
static void decoder_error_exit(j_common_ptr common) __attribute__((noreturn, nonnull(1)));

/**
 * @brief Stops a decode, with a message of its own, by jumping back to `decoder_read`.
 *
 * @param decoder Codec state. Must not be `NULL`.
 * @param format  `printf` format of the message. Must not be `NULL`.
 * @param ...     Arguments `format` names.
 */
static void decoder_fail(struct JpegDecoder* decoder, const char* format, ...)
    __attribute__((noreturn, nonnull(1, 2), format(printf, 2, 3)));

/**
 * @brief Stops a decode whose input has passed `JPEG_SCAN_COUNT_MAX` scans.
 *
 * @param common The codec, whose `client_data` is its `struct JpegDecoder`. Must not be `NULL`.
 */
static void decoder_progress_check(j_common_ptr common) __attribute__((nonnull(1)));

/**
 * @brief Drops a message the library would print, such as a warning about corrupt data.
 *
 * @param common The codec that reports the message.
 */
static void decoder_output_message(j_common_ptr common);

/**
 * @brief Converts one row of Adobe-style inverted CMYK to RGB, multiplying each color by black.
 *
 * @param cmyk     Row of `width_px` CMYK pixels. Must not be `NULL`.
 * @param width_px Pixels in the row.
 * @param rgb      Receives the row as RGB. Must not be `NULL`.
 */
static void cmyk_convert_row(const unsigned char* cmyk, ptrdiff_t width_px, unsigned char* rgb)
    __attribute__((nonnull(1, 3)));

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
                struct ImageHeader* header_out,
                struct Str operation,
                struct Str file_path,
                struct Log* log) {
  const struct ImageReport report = {operation, file_path, log};
  struct JpegDecoder decoder;
  return decoder_read(&decoder, data, header_out, IMAGE_SCALE_FULL, NULL, &report);
}

int image_decode(struct Str data,
                 ptrdiff_t scale_eighths,
                 struct Image* image_out,
                 struct Str operation,
                 struct Str file_path,
                 struct Log* log) {
  *image_out = (struct Image){0};
  const struct ImageReport report = {operation, file_path, log};
  struct JpegDecoder decoder;
  struct ImageHeader header;
  return decoder_read(&decoder, data, &header, scale_eighths, image_out, &report);
}

ptrdiff_t image_decode_len(const struct ImageHeader* header,
                           ptrdiff_t scale_eighths,
                           enum ImageOrientation orientation) {
  ptrdiff_t width = 0;
  ptrdiff_t height = 0;
  image_scale_size(header->width_px, header->height_px, scale_eighths, &width, &height);
  const ptrdiff_t pixel_len = width * height * IMAGE_CHANNEL_COUNT;
  const ptrdiff_t decode_len = header->coefficient_len + pixel_len;
  const ptrdiff_t orient_len = orientation == IMAGE_ORIENTATION_TOP_LEFT ? 0 : 2 * pixel_len;
  return decode_len > orient_len ? decode_len : orient_len;
}

void image_scale_size(ptrdiff_t width_px,
                      ptrdiff_t height_px,
                      ptrdiff_t scale_eighths,
                      ptrdiff_t* width_out,
                      ptrdiff_t* height_out) {
  // libjpeg-turbo rounds each scaled dimension up.
  *width_out = (width_px * scale_eighths + IMAGE_SCALE_FULL - 1) / IMAGE_SCALE_FULL;
  *height_out = (height_px * scale_eighths + IMAGE_SCALE_FULL - 1) / IMAGE_SCALE_FULL;
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

static int decoder_read(struct JpegDecoder* decoder,
                        struct Str data,
                        struct ImageHeader* header_out,
                        ptrdiff_t scale_eighths,
                        struct Image* image_out,
                        const struct ImageReport* report) {
  struct jpeg_decompress_struct* decompress = &decoder->decompress;
  decompress->err = jpeg_std_error(&decoder->error);
  decoder->error.error_exit = decoder_error_exit;
  decoder->error.output_message = decoder_output_message;
  const char* stage = image_out == NULL ? "probe" : "decode";
  if (setjmp(decoder->exit) != 0) {
    jpeg_destroy_decompress(decompress);
    if (image_out != NULL) {
      image_free(image_out);
    }
    return log_error(report->log, "%.*s: failed to %s JPEG: %s ('%.*s')",
                     STR_FMT(report->operation), stage, decoder->message,
                     STR_FMT(report->file_path));
  }

  // Creating the codec keeps `client_data`, so a failure inside it already reaches `decoder`.
  decompress->client_data = decoder;
  jpeg_create_decompress(decompress);
  decoder->progress.progress_monitor = decoder_progress_check;
  decompress->progress = &decoder->progress;
  jpeg_mem_src(decompress, (const unsigned char*)data.data, (unsigned long)data.len);
  (void)jpeg_read_header(decompress, TRUE);
  const int rc = decoder_read_header_check(decoder, header_out, report);
  if (rc == 0 && image_out != NULL) {
    const bool is_cmyk =
        decompress->jpeg_color_space == JCS_CMYK || decompress->jpeg_color_space == JCS_YCCK;
    decompress->scale_num = (unsigned int)scale_eighths;
    decompress->scale_denom = IMAGE_SCALE_FULL;
    decompress->out_color_space = is_cmyk ? JCS_CMYK : JCS_RGB;
    (void)jpeg_start_decompress(decompress);
    decoder_read_rows(decoder, is_cmyk, image_out);
  }
  // Every row is read, so the decode stops here. Reading on to the end marker, as
  // `jpeg_finish_decompress` does, would fail a file whose bytes after its last row are damaged.
  jpeg_destroy_decompress(decompress);
  return rc;
}

static int decoder_read_header_check(struct JpegDecoder* decoder,
                                     struct ImageHeader* header_out,
                                     const struct ImageReport* report) {
  struct jpeg_decompress_struct* decompress = &decoder->decompress;
  if (decompress->data_precision != 8) {
    ERREXIT1(decompress, JERR_BAD_PRECISION, decompress->data_precision);
  }
  const J_COLOR_SPACE color_space = decompress->jpeg_color_space;
  if (color_space != JCS_GRAYSCALE && color_space != JCS_YCbCr && color_space != JCS_RGB &&
      color_space != JCS_CMYK && color_space != JCS_YCCK) {
    ERREXIT(decompress, JERR_CONVERSION_NOTIMPL);
  }
  // Reject hostile dimensions before the decoder allocates for them.
  const ptrdiff_t width = decompress->image_width;
  const ptrdiff_t height = decompress->image_height;
  if (validate_dimensions(width, height, report) != 0) {
    return -1;
  }

  // libjpeg-turbo cannot scale a lossless JPEG, and decodes one at full size at any scale, so its
  // dimensions at the smallest scale are not the ones `image_scale_size` gives.
  decompress->scale_num = 1;
  decompress->scale_denom = IMAGE_SCALE_FULL;
  jpeg_calc_output_dimensions(decompress);
  ptrdiff_t width_scaled = 0;
  ptrdiff_t height_scaled = 0;
  image_scale_size(width, height, 1, &width_scaled, &height_scaled);
  if (decompress->output_width != width_scaled || decompress->output_height != height_scaled) {
    decoder_fail(decoder, "lossless JPEG is unsupported");
  }

  // A multi-scan decode keeps each component's blocks, padded to whole sampling units in each
  // direction, as libjpeg-turbo's coefficient controller allocates them.
  ptrdiff_t coefficient_len = 0;
  if (jpeg_has_multiple_scans(decompress)) {
    for (int i = 0; i < decompress->num_components; i++) {
      const jpeg_component_info* component = &decompress->comp_info[i];
      const ptrdiff_t h_samp = component->h_samp_factor;
      const ptrdiff_t v_samp = component->v_samp_factor;
      const ptrdiff_t width_blocks = ((ptrdiff_t)component->width_in_blocks + h_samp - 1) / h_samp;
      const ptrdiff_t height_blocks =
          ((ptrdiff_t)component->height_in_blocks + v_samp - 1) / v_samp;
      coefficient_len += width_blocks * h_samp * height_blocks * v_samp * (ptrdiff_t)sizeof(JBLOCK);
    }
  }
  header_out->width_px = width;
  header_out->height_px = height;
  header_out->coefficient_len = coefficient_len;
  return 0;
}

static void decoder_read_rows(struct JpegDecoder* decoder, bool is_cmyk, struct Image* image_out) {
  struct jpeg_decompress_struct* decompress = &decoder->decompress;
  const ptrdiff_t width = decompress->output_width;
  const ptrdiff_t height = decompress->output_height;
  image_out->pixels = image_alloc((size_t)(width * height * IMAGE_CHANNEL_COUNT));
  image_out->width_px = width;
  image_out->height_px = height;
  // A CMYK row is decoded into the codec's own pool, which `jpeg_destroy_decompress` releases.
  JSAMPROW cmyk = NULL;
  if (is_cmyk) {
    cmyk =
        (*decompress->mem->alloc_small)((j_common_ptr)decompress, JPOOL_IMAGE, (size_t)width * 4);
  }
  while (decompress->output_scanline < decompress->output_height) {
    unsigned char* row =
        image_out->pixels + (ptrdiff_t)decompress->output_scanline * width * IMAGE_CHANNEL_COUNT;
    JSAMPROW target = is_cmyk ? cmyk : row;
    (void)jpeg_read_scanlines(decompress, &target, 1);
    if (is_cmyk) {
      cmyk_convert_row(cmyk, width, row);
    }
  }
}

static void decoder_error_exit(j_common_ptr common) {
  struct JpegDecoder* decoder = common->client_data;
  if (common->err->msg_code == JERR_OUT_OF_MEMORY) {
    os_oom();
  }
  (*common->err->format_message)(common, decoder->message);
  longjmp(decoder->exit, 1);
}

static void decoder_fail(struct JpegDecoder* decoder, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(decoder->message, sizeof(decoder->message), format, arguments);
  va_end(arguments);
  longjmp(decoder->exit, 1);
}

static void decoder_progress_check(j_common_ptr common) {
  struct JpegDecoder* decoder = common->client_data;
  if (decoder->decompress.input_scan_number > JPEG_SCAN_COUNT_MAX) {
    decoder_fail(decoder, "JPEG exceeds max scan count (%d scans) at %d scans", JPEG_SCAN_COUNT_MAX,
                 decoder->decompress.input_scan_number);
  }
}

static void decoder_output_message(j_common_ptr common) {
  (void)common;
}

static void cmyk_convert_row(const unsigned char* cmyk, ptrdiff_t width_px, unsigned char* rgb) {
  // Each color is the rounded product `color * black / 255`, computed without a division.
  for (ptrdiff_t x = 0; x < width_px; x++) {
    const unsigned int black = cmyk[x * 4 + 3];
    for (ptrdiff_t c = 0; c < IMAGE_CHANNEL_COUNT; c++) {
      const unsigned int product = cmyk[x * 4 + c] * black + 128;
      rgb[x * IMAGE_CHANNEL_COUNT + c] = (unsigned char)((product + (product >> 8)) >> 8);
    }
  }
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
