#ifndef FRAM_TEST_ALBUM_SCANNER_H
#define FRAM_TEST_ALBUM_SCANNER_H

#include "domain/gallery_config.h"
#include "shared/str.h"

// This is header-only and `static inline` so that the in-memory and the video tests of the probe
// phase, two executables, share one configuration without a separate test library to link.

/**
 * @brief Initializes the minimal fixed configuration the scanner tests share.
 *
 * Every string is a borrowed literal, so the caller releases nothing beyond what
 * `gallery_config_init` itself requires.
 *
 * @param gallery_config Configuration to initialize. Must not be `NULL`.
 */
static inline void test_album_scanner_init_config(struct GalleryConfig* gallery_config)
    __attribute__((nonnull(1)));

static inline void test_album_scanner_init_config(struct GalleryConfig* gallery_config) {
  gallery_config_init(gallery_config);
  gallery_config->title = S("Gallery");
  gallery_config->author = S("Author");
  gallery_config->input_dir = S("/photos");
  gallery_config->output_dir = S("/public");
}

#endif
