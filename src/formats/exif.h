#ifndef FRAM_EXIF_H
#define FRAM_EXIF_H

#include "formats/image.h"
#include "shared/str.h"

/**
 * @brief Reads the IFD0 orientation from JPEG EXIF metadata.
 *
 * Malformed metadata, a missing orientation, and values outside the EXIF range all produce
 * `IMAGE_ORIENTATION_TOP_LEFT`.
 *
 * @param data JPEG bytes.
 * @return The parsed orientation, or `IMAGE_ORIENTATION_TOP_LEFT` when none is usable.
 */
enum ImageOrientation exif_read_orientation(struct Str data);

#endif
