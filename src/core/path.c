#include "core/path.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/ascii.h"
#include "shared/arena.h"
#include "shared/str.h"

/**
 * @brief Returns the longest `/`-delimited segment in a path.
 *
 * Each segment becomes one filesystem name, so the check applies a per-filename limit per segment
 * rather than against the whole path's length. A diagnostic that rejects an overlong segment has to
 * name the segment, not just report its length, so this returns the segment rather than only
 * measuring it.
 *
 * It is file-local. `path_check_output_limits` is the only caller, and it reports the segment
 * onward through its `segment_out`, so nothing outside this file needs the scan itself. A test
 * exercises it through that caller rather than through an export.
 *
 * @param path Path to scan.
 * @return View into `path` of the longest segment, the first of equal length. Trailing and empty
 *         segments count as zero, so a path with no segment content yields an empty view.
 */
static struct Str segment_longest(struct Str path);

/**
 * @brief Reports whether a path segment is the `.` or `..` name forbidden in relative paths.
 *
 * @param segment Segment bytes.
 * @return `true` when the segment is exactly `.` or `..`, `false` otherwise.
 */
static bool is_dot_segment(struct Str segment);

enum PathOutputVerdict path_check_output_limits(struct Str relative_path, struct Str* segment_out) {
  *segment_out = segment_longest(relative_path);
  if (relative_path.len > OUTPUT_PATH_RELATIVE_LEN_MAX) {
    return PATH_OUTPUT_TOO_LONG;
  }
  if (segment_out->len > FILENAME_LEN_MAX) {
    return PATH_OUTPUT_SEGMENT_TOO_LONG;
  }
  return PATH_OUTPUT_OK;
}

struct Str path_basename(struct Str path) {
  ptrdiff_t basename_beg = path.len;
  while (basename_beg > 0 && path.data[basename_beg - 1] != '/') {
    basename_beg--;
  }
  return str_tail(path, basename_beg);
}

struct Str path_parent(struct Str path) {
  // The `/` before the basename belongs to neither part.
  const ptrdiff_t basename_len = path_basename(path).len;
  return str_head(path, basename_len == path.len ? 0 : path.len - basename_len - 1);
}

struct Str path_join(struct Str base_path, struct Str relative_path, struct Arena* arena) {
  struct Str joined = str_clone(base_path, arena);
  if (base_path.len > 0 && base_path.data[base_path.len - 1] != '/') {
    joined = str_concat(joined, S("/"), arena);
  }
  return str_concat(joined, relative_path, arena);
}

bool path_is_below(struct Str path, struct Str root_dir, struct Str* relative_path_out) {
  if (!str_startswith(path, root_dir) || path.len == root_dir.len ||
      path.data[root_dir.len] != '/') {
    return false;
  }
  *relative_path_out = str_tail(path, root_dir.len + 1);
  return true;
}

bool path_is_safe_relative(struct Str path) {
  for (ptrdiff_t i = 0; i < path.len; i++) {
    const unsigned char c = (unsigned char)path.data[i];
    if (!ascii_is_alphanumeric(c) && c != '_' && c != '-' && c != '.' && c != '/') {
      return false;
    }
  }
  // An empty path, a leading `/`, and a doubled or trailing `/` each yield an empty segment.
  struct StrCut cut = {.tail = path, .is_found = true};
  while (cut.is_found) {
    cut = str_cut(cut.tail, '/');
    if (cut.head.len == 0 || is_dot_segment(cut.head)) {
      return false;
    }
  }
  return true;
}

static struct Str segment_longest(struct Str path) {
  struct Str longest = str_head(path, 0);
  struct StrCut cut = {.tail = path, .is_found = true};
  while (cut.is_found) {
    cut = str_cut(cut.tail, '/');
    if (cut.head.len > longest.len) {
      longest = cut.head;
    }
  }
  return longest;
}

static bool is_dot_segment(struct Str segment) {
  return str_eq(segment, S(".")) || str_eq(segment, S(".."));
}
