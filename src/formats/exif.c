#include "formats/exif.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shared/str.h"

enum {
  /** Byte introducing every JPEG marker. A run of these before the code is legal padding. */
  JPEG_MARKER_PREFIX = 0xFF,

  /** Start of image. It opens the stream, and a repeat of it carries no segment to skip. */
  JPEG_MARKER_SOI = 0xD8,

  /** Start of scan. Only entropy-coded data follows, so no metadata segment can appear after it. */
  JPEG_MARKER_SOS = 0xDA,

  /** End of image, ending the marker walk. */
  JPEG_MARKER_EOI = 0xD9,

  /** Application segment 1, the segment EXIF metadata is stored in. */
  JPEG_MARKER_APP1 = 0xE1,

  /** IFD0 tag holding the orientation. */
  EXIF_TAG_ORIENTATION = 0x0112,

  /** TIFF field type for a 16-bit unsigned integer, the only type an orientation is read from. */
  TIFF_TYPE_SHORT = 3,
};

/** Byte order used by one TIFF payload. */
enum TiffByteOrder { TIFF_BYTE_ORDER_LITTLE, TIFF_BYTE_ORDER_BIG };

/**
 * @brief Reads a TIFF 16-bit integer after its range has been checked.
 *
 * @param data       Bytes containing the integer. Must not be `NULL`.
 * @param byte_order TIFF byte order.
 * @return The decoded integer.
 */
static uint16_t read_u16(const unsigned char* data, enum TiffByteOrder byte_order)
    __attribute__((nonnull(1)));

/**
 * @brief Reads a TIFF 32-bit integer after its range has been checked.
 *
 * @param data       Bytes containing the integer. Must not be `NULL`.
 * @param byte_order TIFF byte order.
 * @return The decoded integer.
 */
static uint32_t read_u32(const unsigned char* data, enum TiffByteOrder byte_order)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether one APP1 payload holds a usable orientation, and parses it.
 *
 * @param payload         APP1 payload bytes.
 * @param orientation_out Receives a valid orientation when found. Must not be `NULL`.
 * @return `true` when a usable orientation was found, or `false` otherwise.
 */
static bool has_app1_orientation(struct Str payload, enum ImageOrientation* orientation_out)
    __attribute__((nonnull(2)));

enum ImageOrientation exif_read_orientation(struct Str data) {
  const unsigned char* bytes = (const unsigned char*)data.data;
  if (data.len < 2 || bytes[0] != JPEG_MARKER_PREFIX || bytes[1] != JPEG_MARKER_SOI) {
    return IMAGE_ORIENTATION_TOP_LEFT;
  }

  ptrdiff_t offset = 2;
  while (offset < data.len) {
    if (bytes[offset] != JPEG_MARKER_PREFIX) {
      return IMAGE_ORIENTATION_TOP_LEFT;
    }
    while (offset < data.len && bytes[offset] == JPEG_MARKER_PREFIX) {
      offset++;
    }
    if (offset >= data.len) {
      return IMAGE_ORIENTATION_TOP_LEFT;
    }

    const unsigned char marker = bytes[offset++];
    if (marker == JPEG_MARKER_SOS || marker == JPEG_MARKER_EOI) {
      return IMAGE_ORIENTATION_TOP_LEFT;
    }
    if (marker == JPEG_MARKER_SOI || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }
    if (data.len - offset < 2) {
      return IMAGE_ORIENTATION_TOP_LEFT;
    }

    const ptrdiff_t segment_len = ((ptrdiff_t)bytes[offset] << 8) | bytes[offset + 1];
    if (segment_len < 2 || segment_len > data.len - offset) {
      return IMAGE_ORIENTATION_TOP_LEFT;
    }
    if (marker == JPEG_MARKER_APP1) {
      const struct Str payload = str_head(str_tail(data, offset + 2), segment_len - 2);
      enum ImageOrientation orientation = IMAGE_ORIENTATION_TOP_LEFT;
      if (has_app1_orientation(payload, &orientation)) {
        return orientation;
      }
    }
    offset += segment_len;
  }

  return IMAGE_ORIENTATION_TOP_LEFT;
}

static uint16_t read_u16(const unsigned char* data, enum TiffByteOrder byte_order) {
  if (byte_order == TIFF_BYTE_ORDER_LITTLE) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
  }
  return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t read_u32(const unsigned char* data, enum TiffByteOrder byte_order) {
  if (byte_order == TIFF_BYTE_ORDER_LITTLE) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
  }
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) |
         (uint32_t)data[3];
}

static bool has_app1_orientation(struct Str payload, enum ImageOrientation* orientation_out) {
  static const struct Str exif_signature = STR_INIT("Exif\0\0");
  if (payload.len < exif_signature.len + 8 || !str_startswith(payload, exif_signature)) {
    return false;
  }

  const unsigned char* tiff = (const unsigned char*)payload.data + exif_signature.len;
  const ptrdiff_t tiff_len = payload.len - exif_signature.len;
  enum TiffByteOrder byte_order;
  if (tiff[0] == 'I' && tiff[1] == 'I') {
    byte_order = TIFF_BYTE_ORDER_LITTLE;
  } else if (tiff[0] == 'M' && tiff[1] == 'M') {
    byte_order = TIFF_BYTE_ORDER_BIG;
  } else {
    return false;
  }
  if (read_u16(tiff + 2, byte_order) != 42) {
    return false;
  }

  const uint32_t ifd_offset_u32 = read_u32(tiff + 4, byte_order);
  if (ifd_offset_u32 > (uint64_t)tiff_len) {
    return false;
  }
  const ptrdiff_t ifd_offset = (ptrdiff_t)ifd_offset_u32;
  if (tiff_len - ifd_offset < 2) {
    return false;
  }
  const unsigned char* ifd = tiff + ifd_offset;
  const ptrdiff_t entry_count = read_u16(ifd, byte_order);
  if (entry_count > (tiff_len - ifd_offset - 2) / 12) {
    return false;
  }

  for (ptrdiff_t i = 0; i < entry_count; i++) {
    const unsigned char* entry = ifd + 2 + i * 12;
    if (read_u16(entry, byte_order) != EXIF_TAG_ORIENTATION) {
      continue;
    }
    if (read_u16(entry + 2, byte_order) != TIFF_TYPE_SHORT ||
        read_u32(entry + 4, byte_order) != 1) {
      return false;
    }
    const uint16_t value = read_u16(entry + 8, byte_order);
    if (value < IMAGE_ORIENTATION_TOP_LEFT || value > IMAGE_ORIENTATION_LEFT_BOTTOM) {
      return false;
    }
    *orientation_out = (enum ImageOrientation)value;
    return true;
  }
  return false;
}
