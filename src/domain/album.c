#include "domain/album.h"

void album_init(struct Album* album) {
  // No field has a non-zero default. A zeroed arena is a valid initialized arena, which is what
  // `arena_init` writes.
  *album = (struct Album){0};
}

void album_free(struct Album* album) {
  arena_free(&album->arena);
  album_init(album);
}

bool album_is_root(const struct Album* album) {
  return album->source_dir[0] == '\0';
}
