#ifndef FRAM_MEDIA_ITEM_H
#define FRAM_MEDIA_ITEM_H

#include <stdbool.h>
#include <stddef.h>

#include "formats/image.h"
#include "shared/str.h"

/** Kind of source represented by a media item. */
enum MediaKind {
  /** A JPEG image source. */
  MEDIA_KIND_IMAGE,

  /** An MP4 video source. */
  MEDIA_KIND_VIDEO,
};

/** One configured WebP derivative belonging to a media item. */
struct MediaDerivative {
  /** Configured derivative name. */
  struct Str name;

  /** Filesystem path and output-root-relative URL. */
  struct Str path;
  struct Str url;

  /** Presented derivative dimensions. */
  ptrdiff_t width_px;
  ptrdiff_t height_px;
};

/** One image or video shown in an album. */
struct MediaItem {
  /** Source filesystem path. */
  struct Str source_path;

  /** Source basename with underscores displayed as spaces. */
  struct Str title;

  /** Source kind, written by the serial skeleton pass. */
  enum MediaKind kind;

  /** Ordered configured derivatives, in the arena the skeleton was built in. */
  struct MediaDerivative* derivatives;

  /** Number of entries in `derivatives`, one per recipe in the loaded configuration. */
  ptrdiff_t derivative_count;

  /** Copied original filesystem path and output-root-relative URL. */
  struct Str original_path;
  struct Str original_url;

  // Fields below this boundary, plus each derivative's dimensions, are populated by the probe
  // phase. The split is by arena access, not by cost: workers write only scalars and never allocate
  // into the arena the skeleton was built in.

  /** Video duration in milliseconds, or zero for an image. */
  ptrdiff_t duration_ms;

  /** Image orientation, or top-left for an auto-rotated video frame. */
  enum ImageOrientation orientation;

  /** Presented source dimensions after orientation. */
  ptrdiff_t width_px;
  ptrdiff_t height_px;
};

/**
 * @brief Reports whether a path ends with an extension, comparing ASCII case-insensitively.
 *
 * The media listing folds case when it matches its suffixes, so `.JPG` and `.Mp4` reach every
 * classification that uses this.
 *
 * @param path      Path or filename to test.
 * @param extension Extension including its dot, such as `.jpg`.
 * @return `true` when `path` ends with `extension` ignoring ASCII case, otherwise `false`.
 */
bool media_item_has_extension(struct Str path, struct Str extension);

/**
 * @brief Reports whether a path names an MP4 video source.
 *
 * @param path Path or filename to test.
 * @return `true` when `path` ends in `.mp4` in any ASCII letter case, otherwise `false`.
 */
bool media_item_is_video_path(struct Str path);

#endif
