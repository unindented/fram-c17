#include "build/output_path.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "core/path.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"

int output_path_check_limits(struct Str relative_path, struct Str source_label, struct Log* log) {
  struct Str segment;
  switch (path_check_output_limits(relative_path, &segment)) {
    case PATH_OUTPUT_OK:
      return 0;
    case PATH_OUTPUT_TOO_LONG:
      return log_error(
          log,
          "output path for '%.*s' exceeds max output path length (%d bytes) at %td bytes: '%.*s'",
          STR_FMT(source_label), OUTPUT_PATH_RELATIVE_LEN_MAX, relative_path.len,
          STR_FMT(relative_path));
    case PATH_OUTPUT_SEGMENT_TOO_LONG:
      return log_error(log,
                       "output path segment for '%.*s' exceeds max filename length (%d bytes) at "
                       "%td bytes: '%.*s'",
                       STR_FMT(source_label), FILENAME_LEN_MAX, segment.len, STR_FMT(segment));
  }
  // Unreachable: the switch covers every `enum PathOutputVerdict`, and omits `default:` so that
  // `-Wswitch` fails the build when a verdict is added without a case here.
  abort();
}

struct Str output_path_to_root(struct Str relative_path, struct Arena* arena) {
  // An accepted path is at most `OUTPUT_PATH_RELATIVE_LEN_MAX` bytes, so `depth * 3` cannot
  // overflow.
  ptrdiff_t depth = 0;
  for (ptrdiff_t i = 0; i < relative_path.len; i++) {
    depth += relative_path.data[i] == '/';
  }
  char* prefix = arena_new(arena, depth * 3, char);
  for (ptrdiff_t i = 0; i < depth; i++) {
    memcpy(prefix + i * 3, "../", 3);
  }
  return (struct Str){prefix, depth * 3};
}
