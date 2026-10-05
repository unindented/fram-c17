#include <acutest.h>
#include <string.h>

#include "domain/album.h"

// No album field has a non-zero default, so every field starts empty.
static void test_init_sets_empty_state(void) {
  struct Album album;
  album_init(&album);

  TEST_CHECK(album.source_dir == NULL);
  TEST_CHECK(album.slug_path == NULL);
  TEST_CHECK(album.title == NULL);
  TEST_CHECK(album.url_path == NULL);
  TEST_CHECK(album.output_path == NULL);
  TEST_CHECK(album.path_to_root == NULL);
  TEST_CHECK(album.media == NULL);
  TEST_CHECK(album.media_count == 0);
  TEST_CHECK(album.sub_albums == NULL);
  TEST_CHECK(album.sub_album_count == 0);
  TEST_CHECK(album.parent == NULL);
  TEST_CHECK(album.cover == NULL);
  TEST_CHECK(album.item_count_total == 0);
  TEST_CHECK(album.depth == 0);

  album_free(&album);
}

// Freeing releases arena-owned data and leaves the album reusable. The arena append after the free
// exercises the reuse the header promises, with no intervening init.
static void test_free_allows_reuse(void) {
  struct Album album;
  album_init(&album);

  album.title = arena_strdup(&album.arena, "Trips");
  album.media_count = 4;
  album.depth = 2;
  TEST_ASSERT(album.title != NULL);

  album_free(&album);

  TEST_CHECK(album.title == NULL);
  TEST_CHECK(album.media_count == 0);
  TEST_CHECK(album.depth == 0);

  char* reused = arena_strdup(&album.arena, "reused");
  TEST_ASSERT(reused != NULL);
  TEST_CHECK(strcmp(reused, "reused") == 0);

  album_free(&album);
}

// Re-initializing after a free yields a usable arena again.
static void test_reinit_after_free_is_safe(void) {
  struct Album album;
  album_init(&album);
  album_free(&album);

  album_init(&album);
  char* reused = arena_strdup(&album.arena, "reused");
  TEST_ASSERT(reused != NULL);
  TEST_CHECK(strcmp(reused, "reused") == 0);

  album_free(&album);
}

// Only the album with an empty `source_dir` is the root, including a sub-album whose directory is
// named `root album`, which a diagnostic must not confuse with the root.
static void test_is_root_only_for_empty_source_dir(void) {
  struct Album album;
  album_init(&album);

  album.source_dir = "";
  TEST_CHECK(album_is_root(&album));
  album.source_dir = "trips/2024";
  TEST_CHECK(!album_is_root(&album));
  album.source_dir = "root album";
  TEST_CHECK(!album_is_root(&album));

  album_free(&album);
}

TEST_LIST = {
    {"init sets empty state", test_init_sets_empty_state},
    {"free allows reuse", test_free_allows_reuse},
    {"reinit after free is safe", test_reinit_after_free_is_safe},
    {"is root only for empty source dir", test_is_root_only_for_empty_source_dir},
    {NULL, NULL},
};
