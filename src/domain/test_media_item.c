#include <acutest.h>
#include <string.h>

#include "domain/media_item.h"
#include "shared/arena.h"

// RFC 3986 unreserved bytes and path separators are preserved exactly.
static void test_encode_url_preserves_url_path_bytes(void) {
  struct Arena arena;
  arena_init(&arena);
  char* encoded = media_item_encode_url("A-z_0.~/album/photo.jpg", &arena);
  TEST_ASSERT(encoded != NULL);
  TEST_CHECK(strcmp(encoded, "A-z_0.~/album/photo.jpg") == 0);
  arena_free(&arena);
}

// Reserved punctuation, spaces, and non-ASCII UTF-8 bytes are encoded bytewise in uppercase hex.
static void test_encode_url_encodes_every_other_byte(void) {
  struct Arena arena;
  arena_init(&arena);
  char* encoded = media_item_encode_url("a b/%#?\xC3\xA9.jpg", &arena);
  TEST_ASSERT(encoded != NULL);
  TEST_CHECK(strcmp(encoded, "a%20b/%25%23%3F%C3%A9.jpg") == 0);
  arena_free(&arena);
}

// An empty path still produces a valid owned empty string.
static void test_encode_url_accepts_empty_path(void) {
  struct Arena arena;
  arena_init(&arena);
  char* encoded = media_item_encode_url("", &arena);
  TEST_ASSERT(encoded != NULL);
  TEST_CHECK(strcmp(encoded, "") == 0);
  arena_free(&arena);
}

// An extension matches at the end of the path in any ASCII letter case, and a shorter path or a
// different ending does not match.
static void test_has_extension_folds_ascii_case(void) {
  TEST_CHECK(media_item_has_extension("album/photo.jpg", ".jpg"));
  TEST_CHECK(media_item_has_extension("album/PHOTO.JPG", ".jpg"));
  TEST_CHECK(media_item_has_extension(".jpg", ".jpg"));
  TEST_CHECK(!media_item_has_extension("jpg", ".jpg"));
  TEST_CHECK(!media_item_has_extension("album/photo.jpeg", ".jpg"));
  TEST_CHECK(!media_item_has_extension("album/photo.jpg.txt", ".jpg"));
}

// A video path is recognized by its `.mp4` extension in any letter case, and nothing else is.
static void test_is_video_path_matches_mp4_only(void) {
  TEST_CHECK(media_item_is_video_path("clips/a.mp4"));
  TEST_CHECK(media_item_is_video_path("clips/a.MP4"));
  TEST_CHECK(!media_item_is_video_path("clips/a.jpg"));
  TEST_CHECK(!media_item_is_video_path("mp4"));
}

TEST_LIST = {
    {"encode URL preserves URL path bytes", test_encode_url_preserves_url_path_bytes},
    {"encode URL encodes every other byte", test_encode_url_encodes_every_other_byte},
    {"encode URL accepts empty path", test_encode_url_accepts_empty_path},
    {"has extension folds ASCII case", test_has_extension_folds_ascii_case},
    {"is video path matches MP4 only", test_is_video_path_matches_mp4_only},
    {NULL, NULL},
};
