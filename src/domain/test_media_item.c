#include <acutest.h>

#include "domain/media_item.h"
#include "shared/str.h"

// A video path is recognized by its `.mp4` extension in any letter case, and nothing else is.
static void test_is_video_path_matches_mp4_only(void) {
  TEST_CHECK(media_item_is_video_path(S("clips/a.mp4")));
  TEST_CHECK(media_item_is_video_path(S("clips/a.MP4")));
  TEST_CHECK(!media_item_is_video_path(S("clips/a.jpg")));
  TEST_CHECK(!media_item_is_video_path(S("mp4")));
}

TEST_LIST = {
    {"is video path matches MP4 only", test_is_video_path_matches_mp4_only},
    {NULL, NULL},
};
