#include <acutest.h>

#include "domain/media_item.h"
#include "shared/str.h"

// An extension matches at the end of the path in any ASCII letter case, and a shorter path or a
// different ending does not match.
static void test_has_extension_folds_ascii_case(void) {
  TEST_CHECK(media_item_has_extension(S("album/photo.jpg"), S(".jpg")));
  TEST_CHECK(media_item_has_extension(S("album/PHOTO.JPG"), S(".jpg")));
  TEST_CHECK(media_item_has_extension(S(".jpg"), S(".jpg")));
  TEST_CHECK(!media_item_has_extension(S("jpg"), S(".jpg")));
  TEST_CHECK(!media_item_has_extension(S("album/photo.jpeg"), S(".jpg")));
  TEST_CHECK(!media_item_has_extension(S("album/photo.jpg.txt"), S(".jpg")));
}

// A video path is recognized by its `.mp4` extension in any letter case, and nothing else is.
static void test_is_video_path_matches_mp4_only(void) {
  TEST_CHECK(media_item_is_video_path(S("clips/a.mp4")));
  TEST_CHECK(media_item_is_video_path(S("clips/a.MP4")));
  TEST_CHECK(!media_item_is_video_path(S("clips/a.jpg")));
  TEST_CHECK(!media_item_is_video_path(S("mp4")));
}

TEST_LIST = {
    {"has extension folds ASCII case", test_has_extension_folds_ascii_case},
    {"is video path matches MP4 only", test_is_video_path_matches_mp4_only},
    {NULL, NULL},
};
