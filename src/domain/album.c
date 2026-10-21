#include "domain/album.h"

bool album_is_root(const struct Album* album) {
  return album->source_dir.len == 0;
}
