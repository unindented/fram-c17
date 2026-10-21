#include "domain/media_item.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/ascii.h"
#include "shared/str.h"

bool media_item_has_extension(struct Str path, struct Str extension) {
  if (path.len < extension.len) {
    return false;
  }
  struct Str ending = str_tail(path, path.len - extension.len);
  for (ptrdiff_t i = 0; i < extension.len; i++) {
    if (ascii_to_lower((unsigned char)ending.data[i]) !=
        ascii_to_lower((unsigned char)extension.data[i])) {
      return false;
    }
  }
  return true;
}

bool media_item_is_video_path(struct Str path) {
  return media_item_has_extension(path, S(".mp4"));
}
