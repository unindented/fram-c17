#include "core/path.h"

#include <stdint.h>
#include <string.h>

#include "core/ascii.h"
#include "shared/arena.h"

/**
 * @brief Returns the longest `/`-delimited segment in a path, and its length.
 *
 * Each segment becomes one filesystem name, so the check applies a per-filename limit per segment
 * rather than against the whole path's length. A diagnostic that rejects an overlong segment has to
 * name the segment, not just report its length, so this returns the segment rather than only
 * measuring it.
 *
 * It is file-local. `path_check_output_limits` is the only caller, and it reports the measurement
 * onward through `struct PathOutputMetrics`, so nothing outside this file needs the scan itself. A
 * test exercises it through that caller rather than through an export.
 *
 * The returned segment carries no terminator of its own, so a caller must consume it with an
 * explicit length (`%.*s`, `memcmp`) and never hand it to a `str*` function, which would read on
 * through the following segments. Trailing and empty segments count as zero.
 *
 * @param path            Terminated path to scan. Must not be `NULL`.
 * @param segment_len_out Receives the segment's length in bytes. Must not be `NULL`.
 * @return Pointer into `path` at the longest segment, valid to read for `*segment_len_out` bytes,
 *         or `path` itself with a length of `0` when the path has no segment content.
 */
static const char* segment_longest(const char* path, size_t* segment_len_out)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether a path segment is the `.` or `..` name forbidden in relative paths.
 *
 * @param segment     Segment bytes. Must hold at least `segment_len` bytes. Must not be `NULL`.
 * @param segment_len Length of the segment in bytes.
 * @return `true` when the segment is exactly `.` or `..`, `false` otherwise.
 */
static bool is_dot_segment(const char* segment, size_t segment_len) __attribute__((nonnull(1)));

enum PathOutputVerdict path_check_output_limits(const char* relative_path,
                                                struct PathOutputMetrics* metrics_out) {
  metrics_out->len = strlen(relative_path);
  metrics_out->segment = segment_longest(relative_path, &metrics_out->segment_len);
  if (metrics_out->len > OUTPUT_PATH_RELATIVE_LEN_MAX) {
    return PATH_OUTPUT_TOO_LONG;
  }
  if (metrics_out->segment_len > FILENAME_LEN_MAX) {
    return PATH_OUTPUT_SEGMENT_TOO_LONG;
  }
  return PATH_OUTPUT_OK;
}

char* path_join(const char* base_path, const char* relative_path, struct Arena* arena) {
  const size_t base_len = strlen(base_path);
  const size_t relative_len = strlen(relative_path);
  const bool need_slash = base_len > 0 && base_path[base_len - 1] != '/';
  const size_t slash_len = need_slash ? 1 : 0;
  const size_t prefix_len = base_len + slash_len;
  // `size_t` wraps rather than trapping, so the check runs before the total is used as an
  // allocation size. A wrapped sum would allocate a short buffer that the `memcpy` below then runs
  // off the end of. The comparison is `>=` and not `>` so that `joined_len + 1` has room for the
  // terminator without wrapping too.
  if (relative_len >= SIZE_MAX - prefix_len) {
    return NULL;
  }
  const size_t joined_len = prefix_len + relative_len;
  char* joined = arena_alloc(arena, joined_len + 1);
  if (joined == NULL) {
    return NULL;
  }
  memcpy(joined, base_path, base_len);
  size_t pos = base_len;
  if (need_slash) {
    joined[pos++] = '/';
  }
  memcpy(joined + pos, relative_path, relative_len);
  joined[joined_len] = '\0';
  return joined;
}

const char* path_relative_below(const char* path, const char* root_dir) {
  const size_t root_dir_len = strlen(root_dir);
  if (strncmp(root_dir, path, root_dir_len) != 0 || path[root_dir_len] != '/') {
    return NULL;
  }
  return path + root_dir_len + 1;
}

bool path_is_safe_relative(const char* path) {
  if (path == NULL || *path == '\0' || *path == '/') {
    return false;
  }

  const char* segment = path;
  for (const unsigned char* p = (const unsigned char*)path;; p++) {
    const unsigned char c = *p;
    if (c == '\0' || c == '/') {
      const size_t len = (size_t)((const char*)p - segment);
      if (len == 0 || is_dot_segment(segment, len)) {
        return false;
      }
      if (c == '\0') {
        return true;
      }
      segment = (const char*)p + 1;
    } else if (!ascii_is_alphanumeric(c) && c != '_' && c != '-' && c != '.') {
      return false;
    }
  }
}

static const char* segment_longest(const char* path, size_t* segment_len_out) {
  const char* segment_longest = path;
  size_t segment_longest_len = 0;
  const char* segment = path;
  for (const char* p = path;; p++) {
    if (*p == '/' || *p == '\0') {
      const size_t segment_len = (size_t)(p - segment);
      if (segment_len > segment_longest_len) {
        segment_longest_len = segment_len;
        segment_longest = segment;
      }
      if (*p == '\0') {
        *segment_len_out = segment_longest_len;
        return segment_longest;
      }
      segment = p + 1;
    }
  }
}

static bool is_dot_segment(const char* segment, size_t segment_len) {
  return (segment_len == 1 && segment[0] == '.') ||
         (segment_len == 2 && segment[0] == '.' && segment[1] == '.');
}
