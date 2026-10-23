// Seeded random tests over the parsers of untrusted files: EXIF and JPEG headers. Each property
// runs a fixed number of generated cases from `test_rng`, so the pass is deterministic, needs no
// fuzzing engine, and runs everywhere the suite does, under the sanitizers of whichever preset
// builds it. A failing check names the seed and the case, and setting `FRAM_TEST_SEED` to that seed
// replays the failure. It reaches only the input shapes its generators list.

#include <acutest.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "formats/exif.h"
#include "formats/image.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/** Generated cases each property runs. */
enum { RANDOM_CASE_COUNT = 2000 };

/** Source path every generated image is attributed to. */
#define SOURCE_PATH S("a.jpg")

/**
 * @brief Reports whether a log is consistent with the result its action returned.
 *
 * A failure must say why and a success must say nothing, and every error is one line, so a log's
 * line count is its error count.
 *
 * @param log Log the action wrote. Must not be `NULL`.
 * @param rc  Result the action returned.
 * @return `true` when `rc` is `0` with an empty log, or `-1` with one line per counted error.
 */
static bool is_log_consistent(const struct Log* log, int rc) {
  const struct Str text = buf_str(&log->buf);
  ptrdiff_t line_count = 0;
  for (ptrdiff_t i = 0; i < text.len; i++) {
    line_count += text.data[i] == '\n';
  }
  const bool is_terminated = text.len == 0 || text.data[text.len - 1] == '\n';
  return is_terminated && line_count == log->errors &&
         (rc == 0 ? log->errors == 0 : rc == -1 && log->errors > 0);
}

/**
 * @brief Appends `len` random bytes.
 *
 * @param buffer Buffer that receives the bytes. Must not be `NULL`.
 * @param rng    Generator to draw from. Must not be `NULL`.
 * @param len    Number of bytes to append.
 */
static void append_random_bytes(struct Buf* buffer, struct TestRng* rng, ptrdiff_t len) {
  for (ptrdiff_t i = 0; i < len; i++) {
    buf_append_char(buffer, (char)test_rng_below(rng, 256));
  }
}

/**
 * @brief Appends a TIFF integer of `len` bytes in the given byte order.
 *
 * @param buffer           Buffer that receives the integer. Must not be `NULL`.
 * @param value            Value to append. Only its low `len` bytes are written.
 * @param len              Width of the integer in bytes, `2` or `4`.
 * @param is_little_endian Whether the least significant byte comes first.
 */
static void append_tiff_int(struct Buf* buffer,
                            uint32_t value,
                            ptrdiff_t len,
                            bool is_little_endian) {
  for (ptrdiff_t i = 0; i < len; i++) {
    const ptrdiff_t shift = 8 * (is_little_endian ? i : len - 1 - i);
    buf_append_char(buffer, (char)((value >> shift) & 0xFF));
  }
}

/**
 * @brief Appends one marker code after a random run of `0xFF` fill bytes.
 *
 * @param buffer Buffer that receives the marker. Must not be `NULL`.
 * @param rng    Generator to draw from. Must not be `NULL`.
 * @param marker Marker code that follows the fill.
 */
static void append_marker(struct Buf* buffer, struct TestRng* rng, unsigned char marker) {
  const ptrdiff_t fill_len = 1 + test_rng_below(rng, 3);
  for (ptrdiff_t i = 0; i < fill_len; i++) {
    buf_append_char(buffer, (char)0xFF);
  }
  buf_append_char(buffer, (char)marker);
}

/**
 * @brief Appends a segment: its marker, its big-endian length, and its payload.
 *
 * @param buffer  Buffer that receives the segment. Must not be `NULL`.
 * @param rng     Generator for the marker's fill bytes. Must not be `NULL`.
 * @param marker  Segment marker code.
 * @param payload Segment payload, at most `0xFFFF - 2` bytes.
 */
static void append_segment(struct Buf* buffer,
                           struct TestRng* rng,
                           unsigned char marker,
                           struct Str payload) {
  append_marker(buffer, rng, marker);
  append_tiff_int(buffer, (uint32_t)(payload.len + 2), 2, false);
  buf_append(buffer, payload);
}

/**
 * @brief Builds the payload of an EXIF APP1 segment whose IFD0 holds `orientation`.
 *
 * The payload has either byte order, a gap of random bytes before IFD0, other tags of random types
 * around the orientation entry, and random bytes after the IFD.
 *
 * @param rng         Generator to draw from. Must not be `NULL`.
 * @param orientation Orientation the IFD holds.
 * @param arena       Arena that owns the payload. Must not be `NULL`.
 * @return The payload, starting with the `Exif` signature.
 */
static struct Str build_exif_payload(struct TestRng* rng,
                                     enum ImageOrientation orientation,
                                     struct Arena* arena) {
  static const uint32_t other_tags[] = {0x0100, 0x010F, 0x0110, 0x011A, 0x0131, 0x8769};
  const bool is_little_endian = test_rng_below(rng, 2) == 0;
  struct Buf payload = {.arena = arena};
  buf_append(&payload, S("Exif\0\0"));
  buf_append(&payload, is_little_endian ? S("II") : S("MM"));
  append_tiff_int(&payload, 42, 2, is_little_endian);
  const ptrdiff_t gap_len = test_rng_below(rng, 9);
  append_tiff_int(&payload, (uint32_t)(8 + gap_len), 4, is_little_endian);
  append_random_bytes(&payload, rng, gap_len);
  const ptrdiff_t entry_count = 1 + test_rng_below(rng, 4);
  const ptrdiff_t orientation_index = test_rng_below(rng, entry_count);
  append_tiff_int(&payload, (uint32_t)entry_count, 2, is_little_endian);
  for (ptrdiff_t i = 0; i < entry_count; i++) {
    if (i == orientation_index) {
      append_tiff_int(&payload, 0x0112, 2, is_little_endian);
      append_tiff_int(&payload, 3, 2, is_little_endian);
      append_tiff_int(&payload, 1, 4, is_little_endian);
      append_tiff_int(&payload, (uint32_t)orientation, 2, is_little_endian);
      append_tiff_int(&payload, 0, 2, is_little_endian);
    } else {
      const ptrdiff_t tag_count = (ptrdiff_t)(sizeof(other_tags) / sizeof(other_tags[0]));
      append_tiff_int(&payload, other_tags[test_rng_below(rng, tag_count)], 2, is_little_endian);
      append_random_bytes(&payload, rng, 10);
    }
  }
  append_tiff_int(&payload, 0, 4, is_little_endian);
  append_random_bytes(&payload, rng, test_rng_below(rng, 9));
  return buf_str(&payload);
}

/**
 * @brief Builds a JPEG whose metadata holds `orientation` in one well-formed EXIF segment.
 *
 * Before the segment come stand-alone markers, segments of other kinds and an APP1 segment that is
 * not EXIF, each marker after a run of fill bytes. After it come random bytes or nothing. A reader
 * that stops at the orientation never reaches them.
 *
 * @param rng          Generator to draw from. Must not be `NULL`.
 * @param orientation  Orientation the EXIF segment holds.
 * @param arena        Arena that owns the result. Must not be `NULL`.
 * @param exif_end_out Receives the offset one past the EXIF segment. Must not be `NULL`.
 * @return The JPEG bytes.
 */
static struct Str build_exif_jpeg(struct TestRng* rng,
                                  enum ImageOrientation orientation,
                                  struct Arena* arena,
                                  ptrdiff_t* exif_end_out) {
  static const unsigned char standalone_markers[] = {0x01, 0xD0, 0xD7, 0xD8};
  static const unsigned char other_markers[] = {0xC4, 0xDB, 0xE0, 0xE2, 0xFE};
  const ptrdiff_t standalone_count =
      (ptrdiff_t)(sizeof(standalone_markers) / sizeof(standalone_markers[0]));
  const ptrdiff_t other_count = (ptrdiff_t)(sizeof(other_markers) / sizeof(other_markers[0]));
  const struct Str exif_payload = build_exif_payload(rng, orientation, arena);
  struct Buf jpeg = {.arena = arena};
  buf_append(&jpeg, S("\xFF\xD8"));
  const ptrdiff_t segment_count = test_rng_below(rng, 4);
  for (ptrdiff_t i = 0; i < segment_count; i++) {
    switch (test_rng_below(rng, 3)) {
      case 0:
        append_marker(&jpeg, rng, standalone_markers[test_rng_below(rng, standalone_count)]);
        break;
      case 1: {
        struct Buf payload = {.arena = arena};
        append_random_bytes(&payload, rng, test_rng_below(rng, 17));
        append_segment(&jpeg, rng, other_markers[test_rng_below(rng, other_count)],
                       buf_str(&payload));
        break;
      }
      default: {
        struct Buf payload = {.arena = arena};
        buf_append(&payload, S("http://ns.adobe.com/xap/1.0/\0"));
        append_random_bytes(&payload, rng, test_rng_below(rng, 17));
        append_segment(&jpeg, rng, 0xE1, buf_str(&payload));
        break;
      }
    }
  }
  append_segment(&jpeg, rng, 0xE1, exif_payload);
  *exif_end_out = jpeg.len;
  // Half the inputs end with the segment, so a read past the segment is a read past the input.
  append_random_bytes(&jpeg, rng, test_rng_below(rng, 2) == 0 ? 0 : test_rng_below(rng, 25));
  return buf_str(&jpeg);
}

// A JPEG with one well-formed EXIF segment, in either byte order, after stand-alone markers, fill
// bytes, other segments and a non-EXIF APP1 segment, with other tags around the orientation entry
// and random bytes or nothing after the segment, reads back the orientation it was built with.
static void test_exif_reads_random_well_formed_segments(void) {
  struct TestRng rng = test_rng();
  const uint64_t seed = rng.state;
  const struct Arena block = test_arena();
  for (ptrdiff_t i = 0; i < RANDOM_CASE_COUNT; i++) {
    struct Arena arena = block;
    const enum ImageOrientation orientation =
        (enum ImageOrientation)(IMAGE_ORIENTATION_TOP_LEFT + test_rng_below(&rng, 8));
    ptrdiff_t exif_end = 0;
    // Every input this file hands the code under test is copied into an allocation of exactly its
    // length, so the address sanitizer reports a read past its end, which the spare capacity of a
    // `Buf` would hide.
    const struct Str jpeg =
        str_clone(build_exif_jpeg(&rng, orientation, &arena, &exif_end), &arena);
    const enum ImageOrientation actual = exif_read_orientation(jpeg);
    if (!TEST_CHECK_(actual == orientation, "well-formed segment %td reads its orientation", i)) {
      TEST_MSG("seed %" PRIu64 ", expected %d, actual %d, %td bytes", seed, (int)orientation,
               (int)actual, jpeg.len);
      break;
    }
  }
}

/**
 * @brief Builds a random APP1 payload: random bytes, or an EXIF header followed by an IFD whose
 *        offset, entry count and entries are random.
 *
 * The entries draw their tag, type and count from the values an orientation entry needs and from
 * other values, so a reader reaches each of its checks.
 *
 * @param rng   Generator to draw from. Must not be `NULL`.
 * @param arena Arena that owns the payload. Must not be `NULL`.
 * @return The payload.
 */
static struct Str build_exif_payload_random(struct TestRng* rng, struct Arena* arena) {
  struct Buf payload = {.arena = arena};
  if (test_rng_below(rng, 4) == 0) {
    append_random_bytes(&payload, rng, test_rng_below(rng, 48));
    return buf_str(&payload);
  }
  const bool is_little_endian = test_rng_below(rng, 2) == 0;
  buf_append(&payload, S("Exif\0\0"));
  buf_append(&payload, is_little_endian ? S("II") : S("MM"));
  append_tiff_int(&payload, 42, 2, is_little_endian);
  append_tiff_int(&payload, (uint32_t)test_rng_below(rng, 24), 4, is_little_endian);
  append_tiff_int(&payload, (uint32_t)test_rng_below(rng, 6), 2, is_little_endian);
  const ptrdiff_t entry_count = test_rng_below(rng, 6);
  for (ptrdiff_t i = 0; i < entry_count; i++) {
    const bool is_orientation = test_rng_below(rng, 2) == 0;
    append_tiff_int(&payload, is_orientation ? 0x0112 : 0x010F, 2, is_little_endian);
    append_tiff_int(&payload, test_rng_below(rng, 2) == 0 ? 3 : 4, 2, is_little_endian);
    append_tiff_int(&payload, (uint32_t)test_rng_below(rng, 3), 4, is_little_endian);
    append_tiff_int(&payload, (uint32_t)test_rng_below(rng, 10), 2, is_little_endian);
    append_tiff_int(&payload, 0, 2, is_little_endian);
  }
  append_random_bytes(&payload, rng, test_rng_below(rng, 12));
  return buf_str(&payload);
}

/**
 * @brief Builds a JPEG that ends with one random APP1 segment, whose length may not match its
 *        payload.
 *
 * @param rng   Generator to draw from. Must not be `NULL`.
 * @param arena Arena that owns the result. Must not be `NULL`.
 * @return The JPEG bytes.
 */
static struct Str build_exif_jpeg_random(struct TestRng* rng, struct Arena* arena) {
  const struct Str payload = build_exif_payload_random(rng, arena);
  // Half the lengths are off from the payload's by up to three, so they may run past the input.
  const ptrdiff_t len_error = test_rng_below(rng, 2) == 0 ? test_rng_below(rng, 7) - 3 : 0;
  struct Buf jpeg = {.arena = arena};
  buf_append(&jpeg, S("\xFF\xD8\xFF\xE1"));
  append_tiff_int(&jpeg, (uint32_t)(payload.len + 2 + len_error), 2, false);
  buf_append(&jpeg, payload);
  return buf_str(&jpeg);
}

/**
 * @brief Applies up to `mutation_count` random changes to `buffer`: a quarter cut it short, and the
 *        rest overwrite one byte, half of them with a boundary byte and half with a random one.
 *
 * Half the overwrites land in the focus, from `focus_beg` up to `focus_end` or the end of the
 * buffer, whichever comes first. The others, or all of them when the focus is empty, land anywhere
 * before that end.
 *
 * @param buffer         Bytes to change. Must not be `NULL`.
 * @param rng            Generator to draw from. Must not be `NULL`.
 * @param mutation_count Number of changes. The changes stop early once the buffer is empty.
 * @param focus_beg      Offset where the focus begins.
 * @param focus_end      Offset one past the focus. Must be positive.
 * @param boundary_bytes Array of `boundary_count` bytes a parser branches on. Must not be `NULL`.
 * @param boundary_count Number of boundary bytes. Must be positive.
 * @return The lowest offset a change overwrote or cut at, or the starting length when none did.
 */
static ptrdiff_t mutate(struct Buf* buffer,
                        struct TestRng* rng,
                        ptrdiff_t mutation_count,
                        ptrdiff_t focus_beg,
                        ptrdiff_t focus_end,
                        const unsigned char* boundary_bytes,
                        ptrdiff_t boundary_count) {
  ptrdiff_t lowest = buffer->len;
  for (ptrdiff_t i = 0; i < mutation_count && buffer->len > 0; i++) {
    if (test_rng_below(rng, 4) == 0) {
      buffer->len = test_rng_below(rng, buffer->len + 1);
      lowest = buffer->len < lowest ? buffer->len : lowest;
      continue;
    }
    const ptrdiff_t end = buffer->len < focus_end ? buffer->len : focus_end;
    const ptrdiff_t at = end > focus_beg && test_rng_below(rng, 2) == 0
                             ? focus_beg + test_rng_below(rng, end - focus_beg)
                             : test_rng_below(rng, end);
    buffer->data[at] = test_rng_below(rng, 2) == 0
                           ? (char)boundary_bytes[test_rng_below(rng, boundary_count)]
                           : (char)test_rng_below(rng, 256);
    lowest = at < lowest ? at : lowest;
  }
  return lowest;
}

// Random segments that end the input, and well-formed ones with bytes overwritten or cut short,
// read without a crash and without a read past the input, and always give one of the eight
// orientations. A change only after the EXIF segment leaves its orientation as it was. Each input
// is an arena allocation of exactly its length, so the address sanitizer reports a read past it.
static void test_exif_survives_mutated_segments(void) {
  static const unsigned char boundary_bytes[] = {0x00, 0x01, 0x03, 0x08, 0x12,
                                                 0xD9, 0xDA, 0xE1, 0xFF};
  const ptrdiff_t boundary_count = (ptrdiff_t)(sizeof(boundary_bytes) / sizeof(boundary_bytes[0]));
  struct TestRng rng = test_rng();
  const uint64_t seed = rng.state;
  const struct Arena block = test_arena();
  for (ptrdiff_t i = 0; i < RANDOM_CASE_COUNT; i++) {
    struct Arena arena = block;
    const enum ImageOrientation orientation =
        (enum ImageOrientation)(IMAGE_ORIENTATION_TOP_LEFT + test_rng_below(&rng, 8));
    struct Buf bytes = {.arena = &arena};
    bool is_intact = false;
    if (test_rng_below(&rng, 4) == 0) {
      buf_append(&bytes, build_exif_jpeg_random(&rng, &arena));
    } else {
      ptrdiff_t exif_end = 0;
      buf_append(&bytes, build_exif_jpeg(&rng, orientation, &arena, &exif_end));
      const ptrdiff_t mutation_count = 1 + test_rng_below(&rng, 4);
      // Half the changes land after the segment, where they must not matter.
      is_intact = mutate(&bytes, &rng, mutation_count, exif_end, bytes.len, boundary_bytes,
                         boundary_count) >= exif_end;
    }
    const struct Str jpeg = str_clone(buf_str(&bytes), &arena);
    const enum ImageOrientation actual = exif_read_orientation(jpeg);
    const bool is_ok = actual >= IMAGE_ORIENTATION_TOP_LEFT &&
                       actual <= IMAGE_ORIENTATION_LEFT_BOTTOM &&
                       (!is_intact || actual == orientation);
    if (!TEST_CHECK_(is_ok, "mutated segment %td reads a valid orientation", i)) {
      TEST_MSG("seed %" PRIu64 ", built with %d, actual %d, intact %d, %td bytes", seed,
               (int)orientation, (int)actual, is_intact, jpeg.len);
      break;
    }
  }
}

/**
 * @brief Reports whether every line of a log names `file` the way the image messages do.
 *
 * @param log     Log to inspect. Must not be `NULL`.
 * @param file    File every message must be attributed to.
 * @param scratch Scratch for the expected suffix.
 * @return `true` when each line ends with ` ('<file>')`.
 */
static bool is_log_attributed(const struct Log* log, struct Str file, struct Arena scratch) {
  struct Buf suffix = {.arena = &scratch};
  buf_append(&suffix, S(" ('"));
  buf_append(&suffix, file);
  buf_append(&suffix, S("')"));
  struct StrCut cut = {.tail = buf_str(&log->buf)};
  while (cut.tail.len > 0) {
    cut = str_cut(cut.tail, '\n');
    if (!str_endswith(cut.head, buf_str(&suffix))) {
      return false;
    }
  }
  return true;
}

/** A JPEG the probe test mutates, with the layout of its header. */
struct BaseJpeg {
  /** Encoded bytes. */
  struct Str bytes;

  /** Width the JPEG was encoded with. */
  ptrdiff_t width;

  /** Height the JPEG was encoded with. */
  ptrdiff_t height;

  /** Offset of the frame header's marker. */
  ptrdiff_t frame_start;

  /** Offset one past the frame header, whose fields the probe checks against the image limits. */
  ptrdiff_t header_len;
};

/**
 * @brief Encodes a small JPEG of random pixels and finds its frame header.
 *
 * The probe reads every segment up to the first scan, and the frame header is the one whose
 * fields it checks against the image limits. Overwrites land only up to the end of the frame
 * header. The Huffman tables and the scan header after it are only ever cut short.
 *
 * @param rng      Generator to draw from. Must not be `NULL`.
 * @param arena    Arena that owns the bytes. Must not be `NULL`.
 * @param base_out Receives the JPEG on success. Must not be `NULL`.
 * @param log      Log that receives an encoder failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the encode fails or writes no frame header.
 */
static int build_base_jpeg(struct TestRng* rng,
                           struct Arena* arena,
                           struct BaseJpeg* base_out,
                           struct Log* log) {
  enum { BASE_SIDE_MAX = 24 };
  unsigned char pixels[BASE_SIDE_MAX * BASE_SIDE_MAX * IMAGE_CHANNEL_COUNT];
  const ptrdiff_t width = 1 + test_rng_below(rng, BASE_SIDE_MAX);
  const ptrdiff_t height = 1 + test_rng_below(rng, BASE_SIDE_MAX);
  for (ptrdiff_t i = 0; i < width * height * IMAGE_CHANNEL_COUNT; i++) {
    pixels[i] = (unsigned char)test_rng_below(rng, 256);
  }
  const struct Image image = {pixels, width, height};
  struct Str bytes = {0};
  if (image_encode_jpeg(&image, 50 + test_rng_below(rng, 51), arena, &bytes, S("failed to encode"),
                        SOURCE_PATH, log) != 0) {
    return -1;
  }
  const unsigned char* data = (const unsigned char*)bytes.data;
  for (ptrdiff_t i = 0; i + 3 < bytes.len; i++) {
    if (data[i] == 0xFF && data[i + 1] == 0xC0) {
      const ptrdiff_t header_len = i + 2 + ((ptrdiff_t)data[i + 2] << 8 | data[i + 3]);
      if (header_len > bytes.len) {
        return -1;
      }
      *base_out = (struct BaseJpeg){bytes, width, height, i, header_len};
      return 0;
    }
  }
  return -1;
}

// A small JPEG with bytes of its header overwritten, from random values and from the marker and
// length bytes a parser branches on, or cut short, probes without a crash and without a read past
// the input. A probe that fails says why in one line naming the file, one that succeeds says
// nothing and reports dimensions the decoder accepts, and an untouched JPEG reports its own.
static void test_image_probe_survives_mutated_headers(void) {
  static const unsigned char boundary_bytes[] = {0x00, 0x01, 0x80, 0xC0, 0xC2,
                                                 0xC4, 0xD9, 0xDA, 0xDB, 0xFF};
  const ptrdiff_t boundary_count = (ptrdiff_t)(sizeof(boundary_bytes) / sizeof(boundary_bytes[0]));
  enum { BASE_COUNT = 4 };
  struct TestRng rng = test_rng();
  const uint64_t seed = rng.state;
  struct Arena base_arena = test_arena();
  const struct Arena log_block = test_arena();
  struct BaseJpeg bases[BASE_COUNT];
  for (ptrdiff_t b = 0; b < BASE_COUNT; b++) {
    struct Arena log_arena = log_block;
    struct Log log = {.buf = {.arena = &log_arena}};
    if (!TEST_CHECK_(build_base_jpeg(&rng, &base_arena, &bases[b], &log) == 0,
                     "base JPEG %td encodes with a frame header", b)) {
      const struct Str log_text = buf_str(&log.buf);
      TEST_MSG("seed %" PRIu64 ", log: '%.*s'", seed, STR_FMT(log_text));
      return;
    }
  }

  const struct Arena block = test_arena();
  for (ptrdiff_t i = 0; i < RANDOM_CASE_COUNT; i++) {
    struct Arena arena = block;
    struct Arena log_arena = log_block;
    const ptrdiff_t b = test_rng_below(&rng, BASE_COUNT);
    struct Buf bytes = {.arena = &arena};
    buf_append(&bytes, bases[b].bytes);
    const ptrdiff_t mutation_count = test_rng_below(&rng, 5);
    // Half the changes land in the frame header, where every byte is a length, a marker or a field
    // the probe checks.
    mutate(&bytes, &rng, mutation_count, bases[b].frame_start, bases[b].header_len, boundary_bytes,
           boundary_count);
    const struct Str jpeg = str_clone(buf_str(&bytes), &arena);

    struct Log log = {.buf = {.arena = &log_arena}};
    struct ImageHeader header = {0};
    const int rc = image_probe(jpeg, &header, S("failed to probe"), SOURCE_PATH, &log);
    const ptrdiff_t width = header.width_px;
    const ptrdiff_t height = header.height_px;
    bool is_ok = is_log_consistent(&log, rc) && is_log_attributed(&log, SOURCE_PATH, arena);
    if (rc == 0) {
      is_ok = is_ok && width >= 1 && width <= IMAGE_DIMENSION_MAX && height >= 1 &&
              height <= IMAGE_DIMENSION_MAX && width <= IMAGE_PIXEL_COUNT_MAX / height;
    }
    if (mutation_count == 0) {
      is_ok = is_ok && rc == 0 && width == bases[b].width && height == bases[b].height;
    }
    if (!TEST_CHECK_(is_ok, "mutated header %td probes consistently", i)) {
      TEST_MSG("seed %" PRIu64 ", base %td (%td x %td), %td mutations, %td bytes, got %td x %td",
               seed, b, bases[b].width, bases[b].height, mutation_count, jpeg.len, width, height);
      const struct Str log_text = buf_str(&log.buf);
      TEST_MSG("log: '%.*s'", STR_FMT(log_text));
      break;
    }
  }
}

TEST_LIST = {
    {"exif reads random well formed segments", test_exif_reads_random_well_formed_segments},
    {"exif survives mutated segments", test_exif_survives_mutated_segments},
    {"image probe survives mutated headers", test_image_probe_survives_mutated_headers},
    {NULL, NULL},
};
