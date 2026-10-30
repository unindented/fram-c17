#include "domain/build_cache.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "core/fram_version.h"
#include "core/parse.h"
#include "core/path.h"
#include "formats/image.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** First field of a build cache's first line, which names the file kind. */
#define BUILD_CACHE_KIND "fram-build-cache"

/** Nanoseconds in a second, the bound of a row's `mtime_ns`. */
enum { BUILD_CACHE_NS_PER_S = 1000000000 };

/**
 * @brief Parses one row of a build cache, without its newline.
 *
 * @param line      Row to parse.
 * @param entry_out Receives the row, whose path points into `line`. Must not be `NULL`.
 * @return `0` on success, or `-1` when the row does not have its eight fields or one is out of
 *         range.
 */
static int parse_entry(struct Str line, struct BuildCacheEntry* entry_out)
    __attribute__((nonnull(2)));

/**
 * @brief Splits the next tab-separated field off `rest`.
 *
 * @param rest      Text the field is taken from, which receives what follows its tab. Must not be
 *                  `NULL`.
 * @param field_out Receives the field. Must not be `NULL`.
 * @return `true` when a field was taken, or `false` when `rest` was already used up.
 */
static bool take_field(struct StrCut* rest, struct Str* field_out) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders a path against a row's path bytewise, for `bsearch`.
 *
 * @param key     `struct Str` path being looked up. Must not be `NULL`.
 * @param element `struct BuildCacheEntry` row. Must not be `NULL`.
 * @return A negative, zero, or positive value per `str_compare` of the two paths.
 */
static int compare_entry_path(const void* key, const void* element) __attribute__((nonnull(1, 2)));

struct Str build_cache_path(struct Str output_dir, struct Arena* arena) {
  struct Buf name = {.arena = arena};
  buf_printf(&name, "_fram/v%d-build-cache.tsv", BUILD_CACHE_VERSION);
  return path_join(output_dir, buf_str(&name), arena);
}

int build_cache_parse(struct Str text,
                      struct Arena* perm,
                      struct BuildCache* cache_out,
                      struct Log* log) {
  struct Arena arena = *perm;
  struct Arena header_scratch = arena;
  struct Buf header = {.arena = &header_scratch};
  build_cache_format_header(&header);
  if (!str_startswith(text, buf_str(&header))) {
    return log_error(log, "build cache was written by another build");
  }
  const struct Str rows = str_tail(text, buf_str(&header).len);
  ptrdiff_t row_count = 0;
  for (ptrdiff_t i = 0; i < rows.len; i++) {
    row_count += rows.data[i] == '\n';
  }
  struct BuildCache cache = {.entries = arena_new(&arena, row_count, struct BuildCacheEntry)};
  // A file cut short by an interrupted write ends without its last newline, which is the reason
  // every row must end in one.
  struct StrCut cut = str_cut(rows, '\n');
  for (; cut.is_found; cut = str_cut(cut.tail, '\n')) {
    struct BuildCacheEntry* entry = &cache.entries[cache.count];
    if (parse_entry(cut.head, entry) != 0 ||
        (cache.count > 0 && str_compare(cache.entries[cache.count - 1].path, entry->path) >= 0)) {
      return log_error(log, "build cache does not parse at line %td", cache.count + 2);
    }
    entry->path = str_clone(entry->path, &arena);
    cache.count++;
  }
  if (cut.head.len > 0) {
    return log_error(log, "build cache does not parse at line %td", cache.count + 2);
  }
  *perm = arena;
  *cache_out = cache;
  return 0;
}

const struct BuildCacheEntry* build_cache_find(const struct BuildCache* cache,
                                               struct Str path,
                                               const struct OsStat* source_stat) {
  if (cache->count == 0) {
    return NULL;
  }
  const struct BuildCacheEntry* entry = bsearch(&path, cache->entries, (size_t)cache->count,
                                                sizeof(*cache->entries), compare_entry_path);
  if (entry == NULL || entry->len != source_stat->len || entry->mtime_s != source_stat->mtime_s ||
      entry->mtime_ns != source_stat->mtime_ns) {
    return NULL;
  }
  return entry;
}

void build_cache_format_header(struct Buf* buffer) {
  const struct Str generator = fram_generator_string();
  buf_printf(buffer, "%s\t%.*s\n", BUILD_CACHE_KIND, STR_FMT(generator));
}

void build_cache_format_entry(struct Buf* buffer, const struct BuildCacheEntry* entry) {
  for (ptrdiff_t i = 0; i < entry->path.len; i++) {
    if (entry->path.data[i] == '\t' || entry->path.data[i] == '\n') {
      return;
    }
  }
  buf_printf(buffer, "%.*s\t%td\t%" PRId64 "\t%" PRId64 "\t%td\t%td\t%d\t%td\n",
             STR_FMT(entry->path), entry->len, entry->mtime_s, entry->mtime_ns, entry->width_px,
             entry->height_px, (int)entry->orientation, entry->duration_ms);
}

static int parse_entry(struct Str line, struct BuildCacheEntry* entry_out) {
  struct StrCut rest = {.tail = line, .is_found = true};
  struct Str path = {0};
  ptrdiff_t values[7] = {0};
  if (!take_field(&rest, &path) || path.len == 0) {
    return -1;
  }
  for (ptrdiff_t i = 0; i < 7; i++) {
    struct Str field = {0};
    if (!take_field(&rest, &field) || parse_size(field, &values[i]) != 0) {
      return -1;
    }
  }
  const ptrdiff_t orientation = values[5];
  if (rest.is_found || values[2] >= BUILD_CACHE_NS_PER_S || values[3] == 0 || values[4] == 0 ||
      orientation < IMAGE_ORIENTATION_TOP_LEFT || orientation > IMAGE_ORIENTATION_LEFT_BOTTOM) {
    return -1;
  }
  *entry_out = (struct BuildCacheEntry){
      .path = path,
      .len = values[0],
      .mtime_s = values[1],
      .mtime_ns = values[2],
      .width_px = values[3],
      .height_px = values[4],
      .orientation = (enum ImageOrientation)orientation,
      .duration_ms = values[6],
  };
  return 0;
}

static bool take_field(struct StrCut* rest, struct Str* field_out) {
  if (!rest->is_found) {
    return false;
  }
  *rest = str_cut(rest->tail, '\t');
  *field_out = rest->head;
  return true;
}

static int compare_entry_path(const void* key, const void* element) {
  const struct BuildCacheEntry* entry = element;
  return str_compare(*(const struct Str*)key, entry->path);
}
