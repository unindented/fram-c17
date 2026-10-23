#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "formats/image.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_jpeg_fixture.h"
#include "test_support.h"

// A sanitizer's allocator ends the process itself when an allocation fails, so the decode that runs
// out of memory cannot reach libjpeg-turbo's out-of-memory error under one. GCC defines
// `__SANITIZE_*__`, and Apple Clang answers `__has_feature` instead. cppcheck fails on
// `__has_feature` where it does not exist, so the test is hidden from it.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define TEST_IS_SANITIZED 1
#elif defined(__has_feature) && !defined(__CPPCHECK__)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define TEST_IS_SANITIZED 1
#endif
#endif

// Linux enforces `RLIMIT_AS`, and macOS does not, so only Linux can make the decode run out.
#if defined(__linux__) && !defined(TEST_IS_SANITIZED)
#define TEST_HAS_ADDRESS_LIMIT 1
#endif

#ifdef TEST_HAS_ADDRESS_LIMIT
/** JPEG bytes a child process decodes. */
struct DecodeInput {
  /** Encoded JPEG. */
  struct Str data;
};
#endif

/**
 * @brief Encodes one image for decoder and probe tests, recording why when it cannot.
 *
 * @param image    Image to encode. Must not be `NULL`.
 * @param arena    Arena that receives the JPEG bytes. Must not be `NULL`.
 * @param jpeg_out Receives the JPEG, which lives as long as `arena`. Written only on success. Must
 *                 not be `NULL`.
 * @return `0` on success, or `-1` after recording a test failure.
 */
static int encode_fixture(const struct Image* image, struct Arena* arena, struct Str* jpeg_out)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Checks that an image holds one RGB color, within a tolerance, in every pixel.
 *
 * @param image     Image to check. Must not be `NULL`.
 * @param color     Expected RGB color. Must not be `NULL`.
 * @param tolerance Largest difference accepted in any channel.
 */
static void check_solid(const struct Image* image, const unsigned char* color, int tolerance)
    __attribute__((nonnull(1, 2)));

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
                              ptrdiff_t width_expected,
                              ptrdiff_t height_expected) __attribute__((nonnull(2)));

/**
 * @brief Asks `image_alloc` for more bytes than any object can have.
 *
 * @param userdata Unused.
 */
static void alloc_beyond_object_len(void* userdata);

/**
 * @brief Asks `image_realloc` for more bytes than any object can have.
 *
 * @param userdata Unused.
 */
static void realloc_beyond_object_len(void* userdata);

#ifdef TEST_HAS_ADDRESS_LIMIT
/**
 * @brief Decodes a JPEG under an address-space limit that leaves no room for its full-size buffers.
 *
 * The limit leaves 8 MiB above what the process maps already, so the decoder's first full-size
 * buffer cannot be allocated.
 *
 * @param userdata `const struct DecodeInput*` naming the JPEG. Must not be `NULL`.
 */
static void decode_under_address_limit(void* userdata) __attribute__((nonnull(1)));
#endif

static int encode_fixture(const struct Image* image, struct Arena* arena, struct Str* jpeg_out) {
  struct Log log = test_log();
  const int rc = image_encode_jpeg(image, 100, arena, jpeg_out, S("failed to encode fixture"),
                                   S("fixture.jpg"), &log);
  if (!TEST_CHECK(rc == 0)) {
    const struct Str failure = buf_str(&log.buf);
    TEST_MSG("%.*s", STR_FMT(failure));
    return -1;
  }
  return 0;
}

static void check_solid(const struct Image* image, const unsigned char* color, int tolerance) {
  int difference_max = 0;
  for (ptrdiff_t i = 0; i < image->width_px * image->height_px * IMAGE_CHANNEL_COUNT; i++) {
    const int difference = abs((int)image->pixels[i] - (int)color[i % IMAGE_CHANNEL_COUNT]);
    difference_max = difference > difference_max ? difference : difference_max;
  }
  TEST_CHECK_(difference_max <= tolerance, "every channel is within %d of the color", tolerance);
  TEST_MSG("largest difference: %d", difference_max);
}

static void check_orientation(enum ImageOrientation orientation,
                              const unsigned char* expected_values,
                              ptrdiff_t width_expected,
                              ptrdiff_t height_expected) {
  static const unsigned char source_pixels[] = {
      1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6,
  };
  struct Image oriented = {
      .pixels = image_alloc(sizeof(source_pixels)), .width_px = 2, .height_px = 3};
  memcpy(oriented.pixels, source_pixels, sizeof(source_pixels));
  image_orient(&oriented, orientation);
  TEST_CHECK(oriented.width_px == width_expected);
  TEST_CHECK(oriented.height_px == height_expected);
  for (ptrdiff_t i = 0; i < width_expected * height_expected; i++) {
    for (ptrdiff_t channel = 0; channel < IMAGE_CHANNEL_COUNT; channel++) {
      TEST_CHECK(oriented.pixels[i * IMAGE_CHANNEL_COUNT + channel] == expected_values[i]);
    }
  }
  image_free(&oriented);
}

static void alloc_beyond_object_len(void* userdata) {
  (void)userdata;
  (void)image_alloc((size_t)PTRDIFF_MAX + 1);
}

static void realloc_beyond_object_len(void* userdata) {
  (void)userdata;
  (void)image_realloc(NULL, SIZE_MAX);
}

#ifdef TEST_HAS_ADDRESS_LIMIT
static void decode_under_address_limit(void* userdata) {
  const struct DecodeInput* input = userdata;
  FILE* statm = fopen("/proc/self/statm", "r");
  unsigned long mapped_pages = 0;
  if (statm == NULL || fscanf(statm, "%lu", &mapped_pages) != 1) {
    return;
  }
  (void)fclose(statm);
  // The log is reserved before the limit, so only the decode can run out.
  struct Log log = test_log();
  const rlim_t headroom_len = 8 * 1024 * 1024;
  const struct rlimit limit = {
      .rlim_cur = (rlim_t)mapped_pages * (rlim_t)sysconf(_SC_PAGESIZE) + headroom_len,
      .rlim_max = RLIM_INFINITY,
  };
  if (setrlimit(RLIMIT_AS, &limit) != 0) {
    return;
  }
  struct Image image = {0};
  (void)image_decode(input->data, IMAGE_SCALE_FULL, &image, S("failed to decode"), S("a.jpg"),
                     &log);
}
#endif

// A source already inside the box keeps its exact dimensions; fitting never enlarges.
static void test_image_fit_does_not_enlarge(void) {
  ptrdiff_t width = 0;
  ptrdiff_t height = 0;
  image_fit(100, 50, 200, 200, &width, &height);
  TEST_CHECK(width == 100);
  TEST_CHECK(height == 50);
}

// Wide and tall inputs select the limiting side while retaining aspect ratio.
static void test_image_fit_preserves_aspect_ratio(void) {
  ptrdiff_t width = 0;
  ptrdiff_t height = 0;
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
  ptrdiff_t width = 0;
  ptrdiff_t height = 0;
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
  struct Arena arena = test_arena();
  struct Str encoded = {0};
  if (encode_fixture(&source, &arena, &encoded) != 0) {
    return;
  }
  TEST_CHECK(encoded.len > 4);
  TEST_CHECK(str_startswith(encoded, S("\xFF\xD8")));
  TEST_CHECK(str_endswith(encoded, S("\xFF\xD9")));

  struct ImageHeader header;
  struct Log log = test_log();
  TEST_CHECK(image_probe(encoded, &header, S("failed to probe"), S("a.jpg"), &log) == 0);
  TEST_CHECK(header.width_px == 3);
  TEST_CHECK(header.height_px == 2);
  TEST_CHECK(header.coefficient_len == 0);

  struct Image decoded = {0};
  TEST_ASSERT(image_decode(encoded, IMAGE_SCALE_FULL, &decoded, S("failed to decode"), S("a.jpg"),
                           &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(decoded.width_px == 3);
  TEST_CHECK(decoded.height_px == 2);
  for (ptrdiff_t i = 0; i < 6; i++) {
    TEST_CHECK(abs((int)decoded.pixels[i * 3] - 20) <= 3);
    TEST_CHECK(abs((int)decoded.pixels[i * 3 + 1] - 100) <= 3);
    TEST_CHECK(abs((int)decoded.pixels[i * 3 + 2] - 220) <= 3);
  }
  image_free(&decoded);
}

// An encode returns the same JPEG bytes in every run, and they start where `perm` was free, which
// is advanced past them.
static void test_image_encode_is_deterministic_in_perm(void) {
  enum { WIDTH = 32, HEIGHT = 24 };
  unsigned char pixels[WIDTH * HEIGHT * IMAGE_CHANNEL_COUNT];
  for (ptrdiff_t i = 0; i < (ptrdiff_t)sizeof(pixels); i++) {
    pixels[i] = (unsigned char)(i * 37 % 251);
  }
  const struct Image source = {.pixels = pixels, .width_px = WIDTH, .height_px = HEIGHT};
  struct Log log = test_log();
  struct Arena arena = test_arena();
  const char* const perm_beg = arena.beg;
  struct Str first = {0};
  struct Str second = {0};
  TEST_CHECK(image_encode_jpeg(&source, 90, &arena, &first, S("op"), S("a.jpg"), &log) == 0);
  TEST_CHECK(first.data >= perm_beg && arena.beg >= first.data + first.len);
  TEST_CHECK(image_encode_jpeg(&source, 90, &arena, &second, S("op"), S("a.jpg"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(first.len > 2 && (unsigned char)first.data[0] == 0xFF &&
             (unsigned char)first.data[1] == 0xD8);
  TEST_CHECK(str_eq(first, second));
}

// Each scale decodes in the DCT to the dimensions `image_scale_size` gives, rounded up, with the
// colors of the full-size image.
static void test_image_decode_scales_in_the_dct(void) {
  static const unsigned char color[] = {20, 100, 220};
  struct Arena arena = test_arena();
  struct Str encoded = {0};
  if (!TEST_CHECK(test_jpeg_encode_solid(61, 37, color, TEST_JPEG_BASELINE, &arena, &encoded) ==
                  0)) {
    return;
  }
  static const ptrdiff_t widths[] = {0, 8, 16, 23, 31, 39, 46, 54, 61};
  static const ptrdiff_t heights[] = {0, 5, 10, 14, 19, 24, 28, 33, 37};
  for (ptrdiff_t scale_eighths = 1; scale_eighths <= IMAGE_SCALE_FULL; scale_eighths++) {
    ptrdiff_t width = 0;
    ptrdiff_t height = 0;
    image_scale_size(61, 37, scale_eighths, &width, &height);
    TEST_CHECK_(width == widths[scale_eighths] && height == heights[scale_eighths],
                "%td/8 scales 61 x 37 to %td x %td", scale_eighths, width, height);

    struct Image decoded = {0};
    struct Log log = test_log();
    TEST_ASSERT(image_decode(encoded, scale_eighths, &decoded, S("failed to decode"), S("a.jpg"),
                             &log) == 0);
    TEST_CHECK(test_log_is_empty(&log));
    TEST_CHECK_(decoded.width_px == width && decoded.height_px == height,
                "%td/8 decodes to %td x %td", scale_eighths, decoded.width_px, decoded.height_px);
    check_solid(&decoded, color, 4);
    image_free(&decoded);
  }
}

// A CMYK or YCCK JPEG decodes to RGB, each color multiplied by black.
static void test_image_decode_converts_cmyk_and_ycck(void) {
  static const unsigned char color[] = {20, 100, 220};
  static const enum TestJpegKind kinds[] = {TEST_JPEG_CMYK, TEST_JPEG_YCCK};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(kinds) / sizeof(kinds[0])); i++) {
    struct Arena arena = test_arena();
    struct Str encoded = {0};
    if (!TEST_CHECK(test_jpeg_encode_solid(16, 16, color, kinds[i], &arena, &encoded) == 0)) {
      return;
    }
    struct Image decoded = {0};
    struct Log log = test_log();
    TEST_ASSERT(image_decode(encoded, IMAGE_SCALE_FULL, &decoded, S("failed to decode"), S("a.jpg"),
                             &log) == 0);
    TEST_CHECK(test_log_is_empty(&log));
    TEST_CHECK(decoded.width_px == 16 && decoded.height_px == 16);
    check_solid(&decoded, color, 6);
    image_free(&decoded);
  }
}

// A progressive JPEG reports the coefficients a decode buffers for every block of every component:
// a 64 x 48 image with 2 x 2 subsampled chroma has 8 x 6 luma blocks and 4 x 3 blocks for each
// chroma component, 72 blocks of 128 bytes. At 65 x 49 the luma's 9 x 7 blocks are padded to whole
// 2 x 2 sampling units, 10 x 8, beside 5 x 4 for each chroma component, 120 blocks. A progressive
// decode at a reduced scale still decodes.
static void test_image_probe_reports_multi_scan_coefficients(void) {
  static const unsigned char color[] = {200, 40, 90};
  struct Arena arena = test_arena();
  struct Str encoded = {0};
  if (!TEST_CHECK(test_jpeg_encode_solid(64, 48, color, TEST_JPEG_PROGRESSIVE, &arena, &encoded) ==
                  0)) {
    return;
  }
  struct ImageHeader header;
  struct Log log = test_log();
  TEST_ASSERT(image_probe(encoded, &header, S("failed to probe"), S("a.jpg"), &log) == 0);
  TEST_CHECK(header.width_px == 64 && header.height_px == 48);
  TEST_CHECK(header.coefficient_len == 72 * 128);

  struct Str padded = {0};
  if (!TEST_CHECK(test_jpeg_encode_solid(65, 49, color, TEST_JPEG_PROGRESSIVE, &arena, &padded) ==
                  0)) {
    return;
  }
  struct ImageHeader padded_header;
  TEST_ASSERT(image_probe(padded, &padded_header, S("failed to probe"), S("a.jpg"), &log) == 0);
  TEST_CHECK(padded_header.coefficient_len == 120 * 128);

  struct Image decoded = {0};
  TEST_ASSERT(image_decode(encoded, 2, &decoded, S("failed to decode"), S("a.jpg"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(decoded.width_px == 16 && decoded.height_px == 12);
  check_solid(&decoded, color, 4);
  image_free(&decoded);
}

// Damaged bytes after the last row leave every row decoded: here the end marker is replaced by the
// start of another image, which the decoder never reads.
static void test_image_decode_ignores_bytes_after_last_row(void) {
  static const unsigned char color[] = {20, 100, 220};
  struct Arena arena = test_arena();
  struct Str encoded = {0};
  if (!TEST_CHECK(test_jpeg_encode_solid(64, 48, color, TEST_JPEG_BASELINE, &arena, &encoded) ==
                  0)) {
    return;
  }
  TEST_ASSERT(str_endswith(encoded, S("\xFF\xD9")));
  const struct Str damaged = str_concat(str_head(encoded, encoded.len - 2), S("\xFF\xD8"), &arena);

  struct Image decoded = {0};
  struct Log log = test_log();
  TEST_ASSERT(image_decode(damaged, IMAGE_SCALE_FULL, &decoded, S("failed to decode"), S("a.jpg"),
                           &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(decoded.width_px == 64 && decoded.height_px == 48);
  check_solid(&decoded, color, 3);
  image_free(&decoded);
}

// A JPEG that repeats its last scan past 100 scans probes, because the probe reads no scan, and
// fails to decode before it spends the time the scans would take.
static void test_image_decode_rejects_excess_scans(void) {
  static const unsigned char color[] = {20, 100, 220};
  struct Arena arena = test_arena();
  struct Str encoded = {0};
  if (!TEST_CHECK(test_jpeg_encode_solid(16, 16, color, TEST_JPEG_PROGRESSIVE, &arena, &encoded) ==
                  0)) {
    return;
  }
  ptrdiff_t last_scan = -1;
  for (ptrdiff_t i = 0; i + 1 < encoded.len; i++) {
    if ((unsigned char)encoded.data[i] == 0xFF && (unsigned char)encoded.data[i + 1] == 0xDA) {
      last_scan = i;
    }
  }
  TEST_ASSERT(last_scan > 0);
  const struct Str scan = str_head(str_tail(encoded, last_scan), encoded.len - 2 - last_scan);
  struct Buf repeated = {.arena = &arena};
  buf_append(&repeated, str_head(encoded, encoded.len - 2));
  for (ptrdiff_t i = 0; i < 100; i++) {
    buf_append(&repeated, scan);
  }
  buf_append(&repeated, S("\xFF\xD9"));

  struct ImageHeader header;
  struct Log probe_log = test_log();
  TEST_CHECK(
      image_probe(buf_str(&repeated), &header, S("failed to probe"), S("a.jpg"), &probe_log) == 0);
  struct Image decoded = {0};
  struct Log decode_log = test_log();
  TEST_CHECK(image_decode(buf_str(&repeated), IMAGE_SCALE_FULL, &decoded, S("failed to decode"),
                          S("a.jpg"), &decode_log) == -1);
  TEST_CHECK(test_log_is(&decode_log,
                         "failed to decode: failed to decode JPEG: JPEG exceeds max scan count "
                         "(100 scans) at 101 scans ('a.jpg')\n"));
  TEST_CHECK(decoded.pixels == NULL);
}

// Probing rejects hostile dimensions before the decoder can allocate their full pixel buffer.
static void test_image_probe_rejects_oversized_dimensions(void) {
  static const unsigned char jpeg[] = {
      0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x01, 0x4E, 0x21, 0x03, 0x01,
      0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00, 0xFF, 0xDA, 0x00, 0x0C, 0x03,
      0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xFF, 0xD9,
  };
  struct ImageHeader header;
  struct Log probe_log = test_log();
  TEST_CHECK(image_probe(str_from_bytes(jpeg, sizeof(jpeg)), &header, S("failed to probe"),
                         S("a.jpg"), &probe_log) == -1);
  TEST_CHECK(test_log_is(&probe_log,
                         "failed to probe: image exceeds max dimension (20000 pixels) at "
                         "20001 x 1 ('a.jpg')\n"));

  struct Image decoded = {0};
  struct Log decode_log = test_log();
  TEST_CHECK(image_decode(str_from_bytes(jpeg, sizeof(jpeg)), IMAGE_SCALE_FULL, &decoded,
                          S("failed to decode"), S("a.jpg"), &decode_log) == -1);
  TEST_CHECK(test_log_is(&decode_log,
                         "failed to decode: image exceeds max dimension (20000 "
                         "pixels) at 20001 x 1 ('a.jpg')\n"));
  TEST_CHECK(decoded.pixels == NULL);
}

// Dimensions each within `IMAGE_DIMENSION_MAX` whose product passes `IMAGE_PIXEL_COUNT_MAX` are
// rejected too, in the same limit shape: 20000 x 5001 is 100020000 pixels.
static void test_image_probe_rejects_oversized_pixel_count(void) {
  static const unsigned char jpeg[] = {
      0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x13, 0x89, 0x4E, 0x20, 0x03, 0x01,
      0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00, 0xFF, 0xDA, 0x00, 0x0C, 0x03,
      0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xFF, 0xD9,
  };
  struct ImageHeader header;
  struct Log log = test_log();
  TEST_CHECK(image_probe(str_from_bytes(jpeg, sizeof(jpeg)), &header, S("failed to probe"),
                         S("a.jpg"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe: image exceeds max pixel count (100000000 pixels) at "
                         "20000 x 5001 ('a.jpg')\n"));
}

// A header the decoder cannot decode is rejected by the probe, with the reason: 12-bit samples,
// two components, which name no color space, and lossless coding, which cannot be scaled.
static void test_image_probe_rejects_unsupported_headers(void) {
  static const unsigned char twelve_bit[] = {
      0xFF, 0xD8, 0xFF, 0xC1, 0x00, 0x0B, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x11,
      0x00, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0xD9,
  };
  static const unsigned char two_components[] = {
      0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0E, 0x08, 0x00, 0x01, 0x00, 0x01,
      0x02, 0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0xFF, 0xDA, 0x00, 0x0A,
      0x02, 0x01, 0x00, 0x02, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0xD9,
  };
  struct ImageHeader header;
  struct Log precision_log = test_log();
  TEST_CHECK(image_probe(str_from_bytes(twelve_bit, sizeof(twelve_bit)), &header,
                         S("failed to probe"), S("a.jpg"), &precision_log) == -1);
  TEST_CHECK(test_log_is(
      &precision_log,
      "failed to probe: failed to probe JPEG: Unsupported JPEG data precision 12 ('a.jpg')\n"));

  struct Log component_log = test_log();
  TEST_CHECK(image_probe(str_from_bytes(two_components, sizeof(two_components)), &header,
                         S("failed to probe"), S("a.jpg"), &component_log) == -1);
  TEST_CHECK(
      test_log_is(&component_log,
                  "failed to probe: failed to probe JPEG: Unsupported color conversion request "
                  "('a.jpg')\n"));

  static const unsigned char color[] = {20, 100, 220};
  struct Arena arena = test_arena();
  struct Str lossless = {0};
  if (!TEST_CHECK(test_jpeg_encode_solid(16, 16, color, TEST_JPEG_LOSSLESS, &arena, &lossless) ==
                  0)) {
    return;
  }
  struct Log lossless_log = test_log();
  TEST_CHECK(image_probe(lossless, &header, S("failed to probe"), S("a.jpg"), &lossless_log) == -1);
  TEST_CHECK(test_log_is(
      &lossless_log,
      "failed to probe: failed to probe JPEG: lossless JPEG is unsupported ('a.jpg')\n"));
  struct Image decoded = {0};
  struct Log decode_log = test_log();
  TEST_CHECK(image_decode(lossless, IMAGE_SCALE_FULL, &decoded, S("failed to decode"), S("a.jpg"),
                          &decode_log) == -1);
  TEST_CHECK(test_log_is(
      &decode_log,
      "failed to decode: failed to decode JPEG: lossless JPEG is unsupported ('a.jpg')\n"));
  TEST_CHECK(decoded.pixels == NULL);
}

// A decode that runs out of memory inside the codec ends in `os_oom`, which `image_decode` calls
// for libjpeg-turbo's out-of-memory error, rather than failing as an invalid JPEG. The fixture is
// progressive, so the first allocation the limit refuses is one of the codec's own coefficient
// buffers, about 27 MB in all, before any pixels. It needs an address-space limit the system
// enforces, and an allocator that reports a failure, so it runs on Linux without sanitizers.
static void test_image_decode_failure_inside_codec_ends_in_oom(void) {
#ifdef TEST_HAS_ADDRESS_LIMIT
  static const unsigned char black[] = {0, 0, 0};
  struct Arena arena = test_arena();
  struct Str encoded = {0};
  if (!TEST_CHECK(test_jpeg_encode_solid(3000, 3000, black, TEST_JPEG_PROGRESSIVE, &arena,
                                         &encoded) == 0)) {
    return;
  }
  struct DecodeInput input = {.data = encoded};
  TEST_CHECK(test_is_oom(decode_under_address_limit, &input));
#else
  TEST_SKIP("needs an enforced RLIMIT_AS and an unsanitized allocator");
#endif
}

// The region resize, over the whole source, uses the pinned stb v2 sRGB API and returns
// independently owned pixels.
static void test_image_resize_constant_color(void) {
  unsigned char pixels[] = {
      25, 100, 225, 25, 100, 225, 25, 100, 225, 25, 100, 225,
  };
  const struct Image source = {.pixels = pixels, .width_px = 2, .height_px = 2};
  struct Image resized = {0};
  struct Log log = test_log();
  TEST_ASSERT(image_resize_region(&source, 0, 0, 2, 2, 1, 1, &resized, S("failed to resize"),
                                  S("a.jpg"), &log) == 0);
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
  ptrdiff_t x = 0;
  ptrdiff_t y = 0;
  ptrdiff_t width = 0;
  ptrdiff_t height = 0;
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
  struct Log log = test_log();
  TEST_ASSERT(image_resize_region(&source, 1, 0, 2, 2, 2, 2, &resized, S("failed to resize"),
                                  S("a.jpg"), &log) == 0);
  TEST_CHECK(resized.width_px == 2 && resized.height_px == 2);
  for (ptrdiff_t y = 0; y < 2; y++) {
    for (ptrdiff_t channel = 0; channel < IMAGE_CHANNEL_COUNT; channel++) {
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
    ptrdiff_t width = 0;
    ptrdiff_t height = 0;
    image_orient_size((enum ImageOrientation)value, 2, 3, &width, &height);
    TEST_CHECK(width == 2);
    TEST_CHECK(height == 3);
  }
  for (int value = IMAGE_ORIENTATION_LEFT_TOP; value <= IMAGE_ORIENTATION_LEFT_BOTTOM; value++) {
    ptrdiff_t width = 0;
    ptrdiff_t height = 0;
    image_orient_size((enum ImageOrientation)value, 2, 3, &width, &height);
    TEST_CHECK(width == 3);
    TEST_CHECK(height == 2);
  }
}

// Top-left orientation keeps the decoded buffer, so an upright source costs no second full-size
// copy.
static void test_image_orient_top_left_keeps_buffer(void) {
  static const unsigned char source_pixels[] = {1, 2, 3};
  struct Image image = {
      .pixels = image_alloc(sizeof(source_pixels)), .width_px = 1, .height_px = 1};
  memcpy(image.pixels, source_pixels, sizeof(source_pixels));
  unsigned char* const pixels = image.pixels;
  image_orient(&image, IMAGE_ORIENTATION_TOP_LEFT);
  TEST_CHECK(image.pixels == pixels);
  TEST_CHECK(image.width_px == 1);
  TEST_CHECK(image.height_px == 1);
  TEST_CHECK(memcmp(image.pixels, source_pixels, sizeof(source_pixels)) == 0);
  image_free(&image);
}

// A pixel allocation that cannot succeed ends in `os_oom`, as every other allocation does, rather
// than returning `NULL` to a caller that would report a decode error.
static void test_image_alloc_ends_in_oom(void) {
  TEST_CHECK(test_is_oom(alloc_beyond_object_len, NULL));
  TEST_CHECK(test_is_oom(realloc_beyond_object_len, NULL));
}

// Freeing is idempotent and always restores the documented zero state.
static void test_image_free_is_idempotent(void) {
  struct Image image = {.pixels = image_alloc(3), .width_px = 1, .height_px = 1};
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
    {"image encode is deterministic in perm", test_image_encode_is_deterministic_in_perm},
    {"image decode scales in the dct", test_image_decode_scales_in_the_dct},
    {"image decode converts cmyk and ycck", test_image_decode_converts_cmyk_and_ycck},
    {"image probe reports multi scan coefficients",
     test_image_probe_reports_multi_scan_coefficients},
    {"image decode ignores bytes after last row", test_image_decode_ignores_bytes_after_last_row},
    {"image probe rejects oversized dimensions", test_image_probe_rejects_oversized_dimensions},
    {"image probe rejects oversized pixel count", test_image_probe_rejects_oversized_pixel_count},
    {"image probe rejects unsupported headers", test_image_probe_rejects_unsupported_headers},
    {"image decode rejects excess scans", test_image_decode_rejects_excess_scans},
    {"image decode failure inside codec ends in oom",
     test_image_decode_failure_inside_codec_ends_in_oom},
    {"image resize constant color", test_image_resize_constant_color},
    {"image center crop geometry", test_image_center_crop_geometry},
    {"image resize center region", test_image_resize_center_region},
    {"image orient all eight values", test_image_orient_all_eight_values},
    {"image orient size", test_image_orient_size},
    {"image orient top left keeps buffer", test_image_orient_top_left_keeps_buffer},
    {"image alloc ends in oom", test_image_alloc_ends_in_oom},
    {"image free is idempotent", test_image_free_is_idempotent},
    {NULL, NULL},
};
