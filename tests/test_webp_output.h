#ifndef FRAM_TEST_WEBP_OUTPUT_H
#define FRAM_TEST_WEBP_OUTPUT_H

#include "formats/image.h"
#include "shared/str.h"

/**
 * @brief Decodes a WebP output into an RGB image, recording why when it cannot.
 *
 * The decoder is libwebp's, the library of `image_encode_webp`. It allocates the pixels with
 * `malloc`, so the caller releases the image with `image_free`, on success and failure alike. A
 * failure is recorded with acutest's `TEST_MSG`.
 *
 * @param webp      WebP bytes.
 * @param image_out Receives the decoded image on success and is zeroed on failure. Must not be
 *                  `NULL`.
 * @return `0` on success, or `-1` when the bytes are not a WebP image.
 */
int test_webp_decode(struct Str webp, struct Image* image_out) __attribute__((nonnull(2)));

#endif
