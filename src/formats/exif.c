#include "formats/exif.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
 * @brief Parses orientation from one APP1 payload.
 *
 * @param data        APP1 payload bytes. Must not be `NULL`.
 * @param data_len    Number of payload bytes.
 * @param orientation Receives a valid orientation when found. Must not be `NULL`.
 * @return `true` when a usable orientation was found, or `false` otherwise.
 */
static bool parse_app1(const unsigned char* data,
                       size_t data_len,
                       enum ImageOrientation* orientation) __attribute__((nonnull(1, 3)));

enum ImageOrientation exif_read_orientation(const unsigned char* data, size_t data_len) {
  if (data_len < 2 || data[0] != JPEG_MARKER_PREFIX || data[1] != JPEG_MARKER_SOI) {
    return IMAGE_ORIENTATION_TOP_LEFT;
  }

  size_t offset = 2;
  while (offset < data_len) {
    if (data[offset] != JPEG_MARKER_PREFIX) {
      return IMAGE_ORIENTATION_TOP_LEFT;
    }
    while (offset < data_len && data[offset] == JPEG_MARKER_PREFIX) {
      offset++;
    }
    if (offset >= data_len) {
      return IMAGE_ORIENTATION_TOP_LEFT;
    }

    const unsigned char marker = data[offset++];
    if (marker == JPEG_MARKER_SOS || marker == JPEG_MARKER_EOI) {
      return IMAGE_ORIENTATION_TOP_LEFT;
    }
    if (marker == JPEG_MARKER_SOI || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }
    if (data_len - offset < 2) {
      return IMAGE_ORIENTATION_TOP_LEFT;
    }

    const size_t segment_len = ((size_t)data[offset] << 8) | data[offset + 1];
    if (segment_len < 2 || segment_len > data_len - offset) {
      return IMAGE_ORIENTATION_TOP_LEFT;
    }
    const unsigned char* payload = data + offset + 2;
    const size_t payload_len = segment_len - 2;
    if (marker == JPEG_MARKER_APP1) {
      enum ImageOrientation orientation = IMAGE_ORIENTATION_TOP_LEFT;
      if (parse_app1(payload, payload_len, &orientation)) {
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

static bool parse_app1(const unsigned char* data,
                       size_t data_len,
                       enum ImageOrientation* orientation) {
  static const unsigned char exif_signature[] = {'E', 'x', 'i', 'f', 0, 0};
  if (data_len < sizeof(exif_signature) + 8 ||
      memcmp(data, exif_signature, sizeof(exif_signature)) != 0) {
    return false;
  }

  const unsigned char* tiff = data + sizeof(exif_signature);
  const size_t tiff_len = data_len - sizeof(exif_signature);
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
  const size_t ifd_offset = ifd_offset_u32;
  if (ifd_offset > tiff_len || tiff_len - ifd_offset < 2) {
    return false;
  }
  const unsigned char* ifd = tiff + ifd_offset;
  const size_t entry_count = read_u16(ifd, byte_order);
  if (entry_count > (tiff_len - ifd_offset - 2) / 12) {
    return false;
  }

  for (size_t i = 0; i < entry_count; i++) {
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
    *orientation = (enum ImageOrientation)value;
    return true;
  }
  return false;
}
