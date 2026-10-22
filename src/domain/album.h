#ifndef FRAM_ALBUM_H
#define FRAM_ALBUM_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/str.h"

struct MediaItem;

/**
 * One generated gallery page and its immediate contents. The album, its strings, its media records
 * and its pointer arrays all live in the arena the skeleton was built in.
 */
struct Album {
  /** Source directory relative to `input_dir`, or an empty string for the root. */
  struct Str source_dir;

  /** URL-safe generated directory relative to `output_dir`, or empty for the root. */
  struct Str slug_path;

  /** Directory basename, or the gallery title for the root. */
  struct Str title;

  /** Output-root-relative page URL, already URL-safe because it is built from slugs. */
  struct Str url_path;

  /** Filesystem output path. */
  struct Str output_path;

  /** Relative prefix from this page to the output root. */
  struct Str path_to_root;

  /** Media directly in this album, in display order. */
  struct MediaItem** media;

  /** Number of items in `media`, excluding media held by descendant albums. */
  ptrdiff_t media_count;

  /** Immediate child albums, in display order. */
  struct Album** sub_albums;

  /** Number of entries in `sub_albums`, counting immediate children only. */
  ptrdiff_t sub_album_count;

  /** Parent album, or `NULL` for the root. */
  const struct Album* parent;

  /**
   * First direct media item in display order, otherwise the cover of the first child album in
   * display order.
   */
  const struct MediaItem* cover;

  /** Total media count including descendants. */
  ptrdiff_t item_count_total;

  /** Number of directory edges below the root album. */
  ptrdiff_t depth;

  /**
   * Whether the album's page path exceeds an output limit. Its media outputs and its sub-albums
   * extend that path, so the scan reports the album alone and checks none of them.
   */
  bool is_path_rejected;
};

/**
 * @brief Reports whether an album is the gallery's root album.
 *
 * The root album has an empty `source_dir`, which would print as `''`, so a diagnostic names it in
 * words (`the root album`) and names every other album by its quoted `source_dir`. A caller
 * branches on this rather than printing a placeholder name as a value, so a sub-album whose
 * directory is literally `root album` cannot read the same as the root.
 *
 * @param album Album to inspect. Must not be `NULL`, and its `source_dir` must be set.
 * @return `true` when `album` is the root album, `false` otherwise.
 */
bool album_is_root(const struct Album* album) __attribute__((nonnull(1)));

#endif
