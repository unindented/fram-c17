#ifndef FRAM_BUILD_CACHE_H
#define FRAM_BUILD_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "formats/image.h"
#include "shared/str.h"

struct Arena;
struct Buf;
struct Log;
struct OsStat;

/**
 * Format of the build cache's rows, in its file name. Bump it whenever the row format or the
 * meaning of a field changes, so a build never opens a file written in another format, and the
 * stale-output prune removes the old one.
 */
#define BUILD_CACHE_VERSION 1

/** Largest build cache a build reads, in bytes. A larger file is an empty build cache. */
#define BUILD_CACHE_FILE_LEN_MAX ((ptrdiff_t)64 * 1024 * 1024)

/**
 * What the probe read from one media source, reused while the source's size and modification time
 * match. These are the facts about the source itself: derivative dimensions follow from them and
 * the configuration, so editing `fram.toml` never invalidates a row.
 */
struct BuildCacheEntry {
  /** Source path relative to `input_dir`. */
  struct Str path;

  /** Size of the source in bytes when it was probed. */
  ptrdiff_t len;

  /** Modification time of the source when it was probed, in whole seconds. */
  int64_t mtime_s;

  /** Nanoseconds within `mtime_s`. */
  int64_t mtime_ns;

  /** Presented source dimensions after orientation. */
  ptrdiff_t width_px;
  ptrdiff_t height_px;

  /** Image orientation, or top-left for an auto-rotated video frame. */
  enum ImageOrientation orientation;

  /** Video duration in milliseconds, or zero for an image. */
  ptrdiff_t duration_ms;
};

/** The rows of one build cache, sorted bytewise by path. A zeroed build cache is empty. */
struct BuildCache {
  /** Rows, sorted bytewise by `path`, with no path twice. */
  struct BuildCacheEntry* entries;

  /** Number of rows in `entries`. */
  ptrdiff_t count;
};

/**
 * @brief Returns the path of the build cache below `output_dir`, `_fram/v<n>-build-cache.tsv`,
 *        where `<n>` is `BUILD_CACHE_VERSION`.
 *
 * @param output_dir Output directory the build cache lives in.
 * @param arena      Arena that owns the path. Must not be `NULL`.
 * @return The path, owned by `arena`.
 */
struct Str build_cache_path(struct Str output_dir, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Parses a build cache, rejecting a first line that names another build.
 *
 * The first line must name this file kind and the current generator string, so a release that
 * changes how sources are read probes every source again. Every row after it, the last included,
 * ends in a newline, and the rows are sorted bytewise by path with none twice.
 *
 * @param text      Bytes of the build cache.
 * @param perm      Arena that owns the rows. Must not be `NULL`.
 * @param cache_out Receives the rows on success. Written only on success. Must not be `NULL`.
 * @param log       Log that receives the reason the build cache cannot be used. Must not be `NULL`.
 * @return `0` on success, or `-1` when the first line differs or a row does not parse.
 */
int build_cache_parse(struct Str text,
                      struct Arena* perm,
                      struct BuildCache* cache_out,
                      struct Log* log) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Finds the row for `path` whose size and modification time match `source_stat`.
 *
 * @param cache       Build cache to search. Must not be `NULL`.
 * @param path        Source path relative to `input_dir`.
 * @param source_stat What `os_stat` reported for the source now. Must not be `NULL`.
 * @return The matching row, or `NULL` when there is none or the source changed since.
 */
const struct BuildCacheEntry* build_cache_find(const struct BuildCache* cache,
                                               struct Str path,
                                               const struct OsStat* source_stat)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Appends the first line of a build cache for the current generator.
 *
 * @param buffer Buffer that receives the line. Must not be `NULL`.
 */
void build_cache_format_header(struct Buf* buffer) __attribute__((nonnull(1)));

/**
 * @brief Appends one row of a build cache, or nothing for a path that holds a tab or a newline.
 *
 * Such a source has no row, so it is probed on every build, and no path needs escaping.
 *
 * @param buffer Buffer that receives the row. Must not be `NULL`.
 * @param entry  Row to append. Must not be `NULL`.
 */
void build_cache_format_entry(struct Buf* buffer, const struct BuildCacheEntry* entry)
    __attribute__((nonnull(1, 2)));

#endif
