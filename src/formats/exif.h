#ifndef FRAM_EXIF_H
#define FRAM_EXIF_H

#include <stddef.h>

#include "formats/image.h"

/**
 * @brief Reads the IFD0 orientation from JPEG EXIF metadata.
 *
 * Malformed metadata, a missing orientation, and values outside the EXIF range all produce
 * `IMAGE_ORIENTATION_TOP_LEFT`.
 *
 * @param data     JPEG bytes. Must hold at least `data_len` bytes and must not be `NULL`.
 * @param data_len Number of bytes available at `data`.
 * @return The parsed orientation, or `IMAGE_ORIENTATION_TOP_LEFT` when none is usable.
 */
enum ImageOrientation exif_read_orientation(const unsigned char* data, size_t data_len)
    __attribute__((nonnull(1)));

#endif
