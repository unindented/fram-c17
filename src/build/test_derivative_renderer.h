#ifndef FRAM_TEST_DERIVATIVE_RENDERER_H
#define FRAM_TEST_DERIVATIVE_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

#include "domain/gallery_config.h"
#include "domain/manifest.h"
#include "domain/media_item.h"
#include "shared/arena.h"
#include "shared/str.h"

// This is header-only and `static inline` so that the image and the video tests of the derivative
// phase, two executables, share one configuration and one manifest without a separate test library
// to link.

/**
 * @brief Initializes a configuration with the three derivatives each test's `init_item` plans.
 *
 * `s` and `m` crop, and `l` fits. The derivative array is static, so the configuration needs no
 * release and only one may be live at a time.
 *
 * @param config       Configuration to initialize. Must not be `NULL`.
 * @param small_width  Width of the `s` derivative in pixels.
 * @param small_height Height of the `s` derivative in pixels.
 * @param large_width  Width bound of the `l` derivative in pixels.
 * @param large_height Height bound of the `l` derivative in pixels.
 * @param quality      WebP quality shared by every derivative, from 1 through 100.
 */
static inline void test_derivative_renderer_init_config(struct GalleryConfig* config,
                                                        ptrdiff_t small_width,
                                                        ptrdiff_t small_height,
                                                        ptrdiff_t large_width,
                                                        ptrdiff_t large_height,
                                                        ptrdiff_t quality)
    __attribute__((nonnull(1)));

static inline void test_derivative_renderer_init_config(struct GalleryConfig* config,
                                                        ptrdiff_t small_width,
                                                        ptrdiff_t small_height,
                                                        ptrdiff_t large_width,
                                                        ptrdiff_t large_height,
                                                        ptrdiff_t quality) {
  static struct GalleryDerivative derivatives[3];
  derivatives[0] = (struct GalleryDerivative){.name = S("s"),
                                              .width_px = small_width,
                                              .height_px = small_height,
                                              .quality = quality,
                                              .is_crop = true};
  derivatives[1] = (struct GalleryDerivative){
      .name = S("m"), .width_px = 8, .height_px = 4, .quality = quality, .is_crop = true};
  derivatives[2] = (struct GalleryDerivative){.name = S("l"),
                                              .width_px = large_width,
                                              .height_px = large_height,
                                              .quality = quality,
                                              .is_crop = false};
  *config = (struct GalleryConfig){
      .derivatives = derivatives,
      .derivative_count = 3,
  };
}

/**
 * @brief Records every derivative and original path of `media` in `manifest`, as the build's
 *        manifest records them, so the phase's writer accepts each one.
 *
 * Items that share a path record it once.
 *
 * @param manifest    Manifest that receives the paths. Must not be `NULL`.
 * @param media       Media items whose outputs are recorded. Must not be `NULL`.
 * @param media_count Number of items in `media`.
 * @param arena       Arena that owns the manifest's entries. Must not be `NULL`.
 */
static inline void test_derivative_renderer_record_outputs(struct Manifest* manifest,
                                                           struct MediaItem* const* media,
                                                           ptrdiff_t media_count,
                                                           struct Arena* arena)
    __attribute__((nonnull(1, 2, 4)));

static inline void test_derivative_renderer_record_outputs(struct Manifest* manifest,
                                                           struct MediaItem* const* media,
                                                           ptrdiff_t media_count,
                                                           struct Arena* arena) {
  for (ptrdiff_t i = 0; i < media_count; i++) {
    for (ptrdiff_t j = 0; j < media[i]->derivative_count; j++) {
      (void)manifest_add(manifest, media[i]->derivatives[j].path, media[i]->source_path, arena,
                         NULL);
    }
    (void)manifest_add(manifest, media[i]->original_path, media[i]->source_path, arena, NULL);
  }
}

#endif
