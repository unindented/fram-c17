#ifndef FRAM_MEDIA_ITEM_H
#define FRAM_MEDIA_ITEM_H

#include <stdbool.h>
#include <stddef.h>

#include "formats/image.h"

struct Arena;

/** Kind of source represented by a media item. */
enum MediaKind {
  /** A JPEG image source. */
  MEDIA_KIND_IMAGE,

  /** An MP4 video source. */
  MEDIA_KIND_VIDEO,
};

/** One configured JPEG derivative belonging to a media item. */
struct MediaDerivative {
  /** Configured derivative name. */
  const char* name;

  /** Filesystem path and output-root-relative encoded URL. */
  const char* path;
  const char* url;

  /** Presented derivative dimensions. */
  size_t width_px;
  size_t height_px;
};

/** One image or video shown in an album. */
struct MediaItem {
  /** Source filesystem path. */
  const char* source_path;

  /** Source basename with underscores displayed as spaces. */
  const char* title;

  /** Source kind, written by the serial skeleton pass. */
  enum MediaKind kind;

  /** Ordered configured derivatives. Storage belongs to the containing album arena. */
  struct MediaDerivative* derivatives;

  /** Number of entries in `derivatives`, one per recipe in the loaded configuration. */
  size_t derivative_count;

  /** Copied original filesystem path and output-root-relative encoded URL. */
  const char* original_path;
  const char* original_url;

  // Fields below this boundary, plus each derivative's dimensions, are populated by the probe
  // phase. The split is by arena access, not by cost: workers write only scalars and never allocate
  // into an album's arena.

  /** Video duration in milliseconds, or zero for an image. */
  size_t duration_ms;

  /** Image orientation, or top-left for an auto-rotated video frame. */
  enum ImageOrientation orientation;

  /** Presented source dimensions after orientation. */
  size_t width_px;
  size_t height_px;
};

/**
 * @brief Percent-encodes a path for use as an output-root-relative URL.
 *
 * Preserves `/` separators and RFC 3986 unreserved bytes. Every other byte is encoded as an
 * uppercase `%HH` triplet. This is URL encoding only; template rendering applies HTML escaping
 * separately.
 *
 * @param path  Terminated filesystem-relative path to encode. Must not be `NULL`.
 * @param arena Arena that owns the returned URL. Must not be `NULL`.
 * @return Encoded terminated URL owned by `arena`, or `NULL` on overflow or allocation failure.
 */
char* media_item_encode_url(const char* path, struct Arena* arena) __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether a path ends with an extension, comparing ASCII case-insensitively.
 *
 * The media listing folds case when it matches its suffixes, so `.JPG` and `.Mp4` reach every
 * classification that uses this.
 *
 * @param path      Terminated path or filename to test. Must not be `NULL`.
 * @param extension Terminated extension including its dot, such as `.jpg`. Must not be `NULL`.
 * @return `true` when `path` ends with `extension` ignoring ASCII case, otherwise `false`.
 */
bool media_item_has_extension(const char* path, const char* extension)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether a path names an MP4 video source.
 *
 * @param path Terminated path or filename to test. Must not be `NULL`.
 * @return `true` when `path` ends in `.mp4` in any ASCII letter case, otherwise `false`.
 */
bool media_item_is_video_path(const char* path) __attribute__((nonnull(1)));

#endif
