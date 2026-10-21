#include <acutest.h>

#include "domain/album.h"
#include "shared/str.h"

// Only the album with an empty `source_dir` is the root, including a sub-album whose directory is
// named `root album`, which a diagnostic must not confuse with the root.
static void test_is_root_only_for_empty_source_dir(void) {
  struct Album album = {0};

  album.source_dir = S("");
  TEST_CHECK(album_is_root(&album));
  album.source_dir = S("trips/2024");
  TEST_CHECK(!album_is_root(&album));
  album.source_dir = S("root album");
  TEST_CHECK(!album_is_root(&album));
}

TEST_LIST = {
    {"is root only for empty source dir", test_is_root_only_for_empty_source_dir},
    {NULL, NULL},
};
