#ifndef FRAM_ALBUM_H
#define FRAM_ALBUM_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/arena.h"

struct MediaItem;

/** One generated gallery page and its immediate contents. */
struct Album {
  /** Owns all strings, media records, and pointer arrays belonging to this album. */
  struct Arena arena;

  /** Source directory relative to `input_dir`, or an empty string for the root. */
  const char* source_dir;

  /** URL-safe generated directory relative to `output_dir`, or empty for the root. */
  const char* slug_path;

  /** Directory basename, or the gallery title for the root. */
  const char* title;

  /** Percent-encoded output-root-relative page URL. */
  const char* url_path;

  /** Filesystem output path. */
  const char* output_path;

  /** Relative prefix from this page to the output root. */
  const char* path_to_root;

  /** Media directly in this album, sorted by filename. */
  struct MediaItem** media;

  /** Number of items in `media`, excluding media held by descendant albums. */
  size_t media_count;

  /** Immediate child albums. */
  struct Album** sub_albums;

  /** Number of entries in `sub_albums`, counting immediate children only. */
  size_t sub_album_count;

  /** Parent album, or `NULL` for the root. */
  const struct Album* parent;

  /** First direct media item, otherwise the first item below this album. */
  const struct MediaItem* cover;

  /** Total media count including descendants. */
  size_t item_count_total;

  /** Number of directory edges below the root album. */
  size_t depth;
};

/**
 * @brief Initializes an empty album.
 *
 * Prepares the album's arena and resets every field to empty so the album scanner can fill it.
 *
 * @param album Album handle to prepare. Must not be `NULL`, and must not already own arena
 *              allocations. This overwrites the arena handle without releasing it, so a populated
 *              album passed here leaks every chunk it held. Reset a populated album with
 *              `album_free`, which leaves it initialized.
 */
void album_init(struct Album* album) __attribute__((nonnull(1)));

/**
 * @brief Releases arena-owned album data and resets the album for reuse.
 *
 * Invalidates every arena-owned pointer on the album. The album stays initialized, so it may be
 * reused without calling `album_init`.
 *
 * @param album Album to release. Must not be `NULL`.
 */
void album_free(struct Album* album) __attribute__((nonnull(1)));

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
