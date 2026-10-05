#include "build/output_path.h"

#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "core/path.h"
#include "shared/arena.h"

int output_path_check_limits(const char* relative_path,
                             const char* label,
                             char* err,
                             size_t err_len) {
  // The measured length precedes the values, because it is the one part that cannot itself
  // truncate. The label precedes the path, because a path over the whole-path limit is certain to
  // be cut off at the end of the buffer.
  struct PathOutputMetrics metrics;
  switch (path_check_output_limits(relative_path, &metrics)) {
    case PATH_OUTPUT_OK:
      return 0;
    case PATH_OUTPUT_TOO_LONG:
      return error_report(err, err_len,
                          "output path exceeds max output path length (%zu bytes) at %zu bytes "
                          "(for '%s'): '%s'",
                          (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX, metrics.len, label, relative_path);
    case PATH_OUTPUT_SEGMENT_TOO_LONG:
      return error_report(err, err_len,
                          "output path segment exceeds max filename length (%zu bytes) at %zu "
                          "bytes (for '%s'): '%.*s'",
                          (size_t)FILENAME_LEN_MAX, metrics.segment_len, label,
                          (int)metrics.segment_len, metrics.segment);
  }
  // Unreachable: the switch covers every `enum PathOutputVerdict`, and omits `default:` so that
  // `-Wswitch` fails the build when a verdict is added without a case here.
  abort();
}

char* output_path_to_root(const char* relative_path, struct Arena* arena) {
  // An accepted path is at most `OUTPUT_PATH_RELATIVE_LEN_MAX` bytes, so `depth * 3` cannot
  // overflow.
  size_t depth = 0;
  for (const char* p = relative_path; *p != '\0'; p++) {
    depth += *p == '/';
  }
  char* prefix = arena_alloc(arena, depth * 3 + 1);
  if (prefix == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < depth; i++) {
    memcpy(prefix + i * 3, "../", 3);
  }
  prefix[depth * 3] = '\0';
  return prefix;
}
