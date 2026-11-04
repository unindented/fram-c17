#include "domain/media_item.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/text.h"
#include "shared/str.h"

bool media_item_is_video_path(struct Str path) {
  return text_has_suffix_folded(path, S(".mp4"));
}
