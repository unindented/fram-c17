#include "test_jpeg_fixture.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_NO_MAIN
#include <acutest.h>

// `jpeglib.h` uses `size_t` and `FILE` without including their headers.
#include "jpeglib.h"

/**
 * One run of the encoder, which the library reaches through `client_data`. `test_jpeg_encode_run`
 * calls `setjmp`, so the state it changes lives in its caller's frame, where the jump back leaves
 * its value defined.
 */
struct TestJpegEncoder {
  /** The library's compression state. */
  struct jpeg_compress_struct compress;

  /** The library's error manager. Its `error_exit` jumps to `exit` and does not return. */
  struct jpeg_error_mgr error;

  /** Where a fatal error jumps to. */
  jmp_buf exit;

  /** The JPEG, which the library's in-memory destination allocates with `malloc`. */
  unsigned char* output;

  /** Bytes in `output`. */
  unsigned long output_len;
};

/**
 * @brief Runs the encoder over one image into the encoder's output.
 *
 * @param encoder Encoder state, in the caller's frame. Must not be `NULL`.
 * @param image   Image to encode. Must not be `NULL`.
 * @param kind    Color space and scan structure to encode with.
 * @param quality JPEG quality from 1 through 100.
 * @param row     Buffer for one row of four-channel samples. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording the library's message.
 */
static int test_jpeg_encode_run(struct TestJpegEncoder* encoder,
                                const struct Image* image,
                                enum TestJpegKind kind,
                                ptrdiff_t quality,
                                unsigned char* row) __attribute__((nonnull(1, 2, 5)));

/**
 * @brief Records the library's message for a fatal error and jumps back to the encoder.
 *
 * @param common The codec, whose `client_data` is its `struct TestJpegEncoder`. Must not be `NULL`.
 */
static void test_jpeg_error_exit(j_common_ptr common) __attribute__((noreturn, nonnull(1)));

int test_jpeg_encode(ptrdiff_t width_px,
                     ptrdiff_t height_px,
                     struct Arena* arena,
                     struct Str* jpeg_out) {
  const ptrdiff_t pixel_len = width_px * height_px * IMAGE_CHANNEL_COUNT;
  struct Image image = {
      .pixels = image_alloc((size_t)pixel_len), .width_px = width_px, .height_px = height_px};
  for (ptrdiff_t i = 0; i < pixel_len; i++) {
    image.pixels[i] = (unsigned char)((i * 37) % 251);
  }
  const int rc = test_jpeg_encode_image(&image, TEST_JPEG_BASELINE, 90, arena, jpeg_out);
  image_free(&image);
  return rc;
}

int test_jpeg_encode_image(const struct Image* image,
                           enum TestJpegKind kind,
                           ptrdiff_t quality,
                           struct Arena* arena,
                           struct Str* jpeg_out) {
  unsigned char* row = arena_new(arena, image->width_px * 4, unsigned char);
  struct TestJpegEncoder encoder = {0};
  const int rc = test_jpeg_encode_run(&encoder, image, kind, quality, row);
  if (rc == 0) {
    *jpeg_out =
        str_clone((struct Str){(char*)encoder.output, (ptrdiff_t)encoder.output_len}, arena);
  }
  free(encoder.output);
  return rc;
}

int test_jpeg_encode_solid(ptrdiff_t width_px,
                           ptrdiff_t height_px,
                           const unsigned char* color,
                           enum TestJpegKind kind,
                           struct Arena* arena,
                           struct Str* jpeg_out) {
  const ptrdiff_t pixel_count = width_px * height_px;
  struct Image image = {.pixels = image_alloc((size_t)(pixel_count * IMAGE_CHANNEL_COUNT)),
                        .width_px = width_px,
                        .height_px = height_px};
  for (ptrdiff_t i = 0; i < pixel_count; i++) {
    memcpy(image.pixels + i * IMAGE_CHANNEL_COUNT, color, IMAGE_CHANNEL_COUNT);
  }
  const int rc = test_jpeg_encode_image(&image, kind, 95, arena, jpeg_out);
  image_free(&image);
  return rc;
}

static int test_jpeg_encode_run(struct TestJpegEncoder* encoder,
                                const struct Image* image,
                                enum TestJpegKind kind,
                                ptrdiff_t quality,
                                unsigned char* row) {
  struct jpeg_compress_struct* compress = &encoder->compress;
  compress->err = jpeg_std_error(&encoder->error);
  encoder->error.error_exit = test_jpeg_error_exit;
  if (setjmp(encoder->exit) != 0) {
    jpeg_destroy_compress(compress);
    return -1;
  }

  // Creating the codec keeps `client_data`, so a failure inside it already reaches `encoder`.
  compress->client_data = encoder;
  jpeg_create_compress(compress);
  jpeg_mem_dest(compress, &encoder->output, &encoder->output_len);
  const bool is_cmyk = kind == TEST_JPEG_CMYK || kind == TEST_JPEG_YCCK;
  compress->image_width = (JDIMENSION)image->width_px;
  compress->image_height = (JDIMENSION)image->height_px;
  compress->input_components = is_cmyk ? 4 : IMAGE_CHANNEL_COUNT;
  compress->in_color_space = is_cmyk ? JCS_CMYK : JCS_RGB;
  jpeg_set_defaults(compress);
  jpeg_set_quality(compress, (int)quality, TRUE);
  if (kind == TEST_JPEG_YCCK) {
    jpeg_set_colorspace(compress, JCS_YCCK);
  } else if (kind == TEST_JPEG_PROGRESSIVE) {
    jpeg_simple_progression(compress);
  } else if (kind == TEST_JPEG_LOSSLESS) {
    jpeg_set_colorspace(compress, JCS_RGB);
    jpeg_enable_lossless(compress, 1, 0);
  }

  jpeg_start_compress(compress, TRUE);
  while (compress->next_scanline < compress->image_height) {
    unsigned char* rgb =
        image->pixels + (ptrdiff_t)compress->next_scanline * image->width_px * IMAGE_CHANNEL_COUNT;
    JSAMPROW samples = rgb;
    if (is_cmyk) {
      // The largest channel is the black, and each color is scaled up to it, so
      // `color * black / 255` rounds back to the channel.
      for (ptrdiff_t x = 0; x < image->width_px; x++) {
        const unsigned char* pixel = rgb + x * IMAGE_CHANNEL_COUNT;
        int black = pixel[0] > pixel[1] ? pixel[0] : pixel[1];
        black = black > pixel[2] ? black : pixel[2];
        for (ptrdiff_t c = 0; c < IMAGE_CHANNEL_COUNT; c++) {
          row[x * 4 + c] = black == 0 ? 0 : (unsigned char)((pixel[c] * 255 + black / 2) / black);
        }
        row[x * 4 + 3] = (unsigned char)black;
      }
      samples = row;
    }
    (void)jpeg_write_scanlines(compress, &samples, 1);
  }
  jpeg_finish_compress(compress);
  jpeg_destroy_compress(compress);
  return 0;
}

static void test_jpeg_error_exit(j_common_ptr common) {
  struct TestJpegEncoder* encoder = common->client_data;
  char message[JMSG_LENGTH_MAX];
  (*common->err->format_message)(common, message);
  TEST_MSG("failed to encode fixture JPEG: %s", message);
  longjmp(encoder->exit, 1);
}
