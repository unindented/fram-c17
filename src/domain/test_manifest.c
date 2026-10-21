#include <acutest.h>
#include <stddef.h>

#include "domain/manifest.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * @brief Builds the `index`-th of a family of numbered strings, such as `public/7.html`.
 *
 * @param prefix Bytes before the number.
 * @param index  Number in the middle.
 * @param suffix Bytes after the number.
 * @param arena  Arena that owns the returned string. Must not be `NULL`.
 * @return `prefix`, `index` in decimal, then `suffix`, owned by `arena`.
 */
static struct Str numbered_path(struct Str prefix,
                                ptrdiff_t index,
                                struct Str suffix,
                                struct Arena* arena) {
  struct Buf buffer = {.arena = arena};
  buf_printf(&buffer, "%.*s%td%.*s", STR_FMT(prefix), index, STR_FMT(suffix));
  return buf_str(&buffer);
}

// A new path is inserted. Re-adding it is reported as a duplicate that leaves the manifest
// unchanged and surfaces the earlier source label, while a distinct path is still accepted. The
// final re-add takes the documented `NULL` form of `source_label_existing_out`, which is the only
// call in the suite that reaches the `!= NULL` guard's false arm.
static void test_inserts_then_detects_duplicate(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};

  TEST_CHECK(manifest_add(&manifest, S("public/index.html"), S("media"), &arena, NULL) == 0);
  TEST_CHECK(manifest.count == 1);

  struct Str source_label_existing = {0};
  TEST_CHECK(manifest_add(&manifest, S("public/index.html"), S("index.html"), &arena,
                          &source_label_existing) == -1);
  TEST_CHECK(str_eq(source_label_existing, S("media")));
  TEST_CHECK(manifest.count == 1);

  TEST_CHECK(
      manifest_add(&manifest, S("public/map.xml"), S("aggregate_templates[0]"), &arena, NULL) == 0);
  TEST_CHECK(manifest.count == 2);

  TEST_CHECK(manifest_add(&manifest, S("public/index.html"), S("index.html"), &arena, NULL) == -1);
  TEST_CHECK(manifest.count == 2);
}

// Paths that differ only in ASCII case are one output, because a case-insensitive filesystem such
// as the macOS default stores them as one file. The duplicate reports the first claim's label and
// leaves the manifest unchanged. Bytes outside ASCII compare exactly: the UTF-8 encoding of a
// precomposed lowercase e-acute (`C3 A9`) is neither folded to nor normalized into the ASCII `e`,
// and the uppercase E-acute (`C3 89`) is not folded to its lowercase form either.
static void test_detects_ascii_folded_duplicate(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};

  TEST_CHECK(manifest_add(&manifest, S("public/Trips/Japan.JPG"), S("Trips/Japan.JPG"), &arena,
                          NULL) == 0);

  struct Str source_label_existing = {0};
  TEST_CHECK(manifest_add(&manifest, S("public/trips/japan.jpg"), S("trips/japan.jpg"), &arena,
                          &source_label_existing) == -1);
  TEST_CHECK(str_eq(source_label_existing, S("Trips/Japan.JPG")));
  TEST_CHECK(manifest.count == 1);

  TEST_CHECK(manifest_add(&manifest, S("public/caf\xC3\xA9.html"), S("cafe-composed.jpg"), &arena,
                          NULL) == 0);
  TEST_CHECK(manifest_add(&manifest, S("public/cafe.html"), S("cafe-ascii.jpg"), &arena, NULL) ==
             0);
  TEST_CHECK(manifest_add(&manifest, S("public/caf\xC3\x89.html"), S("cafe-upper.jpg"), &arena,
                          NULL) == 0);
  TEST_CHECK(manifest.count == 4);
}

// Enough paths to fill several levels of the hash trie are each still found, and each maps back to
// its own source label. The label captured on the first insert is read again after the rest, which
// exercises the promise `manifest.h` makes for `source_label_existing_out`: the label lives as long
// as `arena`. Reading a label on the line after the call that set it, as the loop does, cannot
// distinguish that promise from a scratch-buffer API.
static void test_many_paths_stay_findable(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};

  enum { ENTRY_COUNT = 1000 };
  TEST_CHECK(manifest_add(&manifest, S("public/0.html"), S("0.jpg"), &arena, NULL) == 0);
  struct Str source_label_held = {0};
  TEST_CHECK(manifest_add(&manifest, S("public/0.html"), S("again.jpg"), &arena,
                          &source_label_held) == -1);
  TEST_CHECK(str_eq(source_label_held, S("0.jpg")));

  for (ptrdiff_t i = 1; i < ENTRY_COUNT; i++) {
    const struct Str output_path = numbered_path(S("public/"), i, S(".html"), &arena);
    const struct Str source_label = numbered_path(S(""), i, S(".jpg"), &arena);
    TEST_CHECK(manifest_add(&manifest, output_path, source_label, &arena, NULL) == 0);
  }
  TEST_CHECK(manifest.count == ENTRY_COUNT);

  for (ptrdiff_t i = 0; i < ENTRY_COUNT; i++) {
    const struct Str output_path = numbered_path(S("public/"), i, S(".html"), &arena);
    const struct Str source_label = numbered_path(S(""), i, S(".jpg"), &arena);
    struct Str source_label_existing = {0};
    TEST_CHECK(
        manifest_add(&manifest, output_path, S("again.jpg"), &arena, &source_label_existing) == -1);
    TEST_CHECK(str_eq(source_label_existing, source_label));
  }
  TEST_CHECK(manifest.count == ENTRY_COUNT);
  TEST_CHECK(str_eq(source_label_held, S("0.jpg")));
}

// A `NUL` byte is an ordinary path byte to the manifest, so two paths that differ only after one
// are distinct, and a path is not equal to its prefix before the `NUL`.
static void test_nul_byte_is_ordinary(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};

  TEST_CHECK(manifest_add(&manifest, S("a\0b"), S("b.jpg"), &arena, NULL) == 0);
  TEST_CHECK(manifest_add(&manifest, S("a\0c"), S("c.jpg"), &arena, NULL) == 0);
  TEST_CHECK(manifest_add(&manifest, S("a"), S("a.jpg"), &arena, NULL) == 0);
  struct Str source_label_existing = {0};
  TEST_CHECK(manifest_add(&manifest, S("a\0b"), S("again.jpg"), &arena, &source_label_existing) ==
             -1);
  TEST_CHECK(str_eq(source_label_existing, S("b.jpg")));
  TEST_CHECK(manifest.count == 3);
}

// The trie is keyed by the folded path, not the recorded bytes, so across enough entries to fill
// several trie levels an uppercase path is still found by its lowercase spelling and maps back to
// its own source label. A trie that descended by an unfolded hash would send the lowercase spelling
// down slots the uppercase one was never placed in.
static void test_deep_trie_preserves_folded_lookup(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};

  enum { ENTRY_COUNT = 1000 };
  for (ptrdiff_t i = 0; i < ENTRY_COUNT; i++) {
    const struct Str output_path = numbered_path(S("PUBLIC/"), i, S(".HTML"), &arena);
    const struct Str source_label = numbered_path(S(""), i, S(".jpg"), &arena);
    TEST_CHECK(manifest_add(&manifest, output_path, source_label, &arena, NULL) == 0);
  }
  TEST_CHECK(manifest.count == ENTRY_COUNT);

  for (ptrdiff_t i = 0; i < ENTRY_COUNT; i++) {
    const struct Str output_path = numbered_path(S("public/"), i, S(".html"), &arena);
    const struct Str source_label = numbered_path(S(""), i, S(".jpg"), &arena);
    struct Str source_label_existing = {0};
    TEST_CHECK(
        manifest_add(&manifest, output_path, S("again.jpg"), &arena, &source_label_existing) == -1);
    TEST_CHECK(str_eq(source_label_existing, source_label));
  }
  TEST_CHECK(manifest.count == ENTRY_COUNT);
}

// `manifest_add` records `a/b` and `a/b/c` as distinct, because neither equals the other, but the
// two cannot coexist on disk: `a/b` is a file that `a/b/c` needs to be a directory.
// `manifest_find_prefix_collision` catches that pair, naming the shorter path as the ancestor (the
// file) and the longer as the descendant.
static void test_detects_prefix_collision(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};

  TEST_CHECK(manifest_add(&manifest, S("a/b"), S("p.jpg"), &arena, NULL) == 0);
  TEST_CHECK(manifest_add(&manifest, S("a/b/c"), S("q.jpg"), &arena, NULL) == 0);
  TEST_CHECK(manifest.count == 2);

  struct ManifestPrefixCollision collision = {0};
  TEST_CHECK(manifest_find_prefix_collision(&manifest, &collision));
  TEST_CHECK(str_eq(collision.ancestor_path, S("a/b")));
  TEST_CHECK(str_eq(collision.ancestor_label, S("p.jpg")));
  TEST_CHECK(str_eq(collision.descendant_path, S("a/b/c")));
  TEST_CHECK(str_eq(collision.descendant_label, S("q.jpg")));
}

// The ancestor lookup folds ASCII case the same way `manifest_add` does, so a file `Assets/Icon`
// collides with `assets/icon/image.jpg`, which on a case-insensitive filesystem needs that same
// name as a directory. The collision reports each path in the case it was recorded in.
static void test_detects_ascii_folded_prefix_collision(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};

  TEST_CHECK(manifest_add(&manifest, S("public/Assets/Icon"), S("Icon.jpg"), &arena, NULL) == 0);
  TEST_CHECK(manifest_add(&manifest, S("public/assets/icon/image.jpg"), S("image.jpg"), &arena,
                          NULL) == 0);

  struct ManifestPrefixCollision collision = {0};
  TEST_CHECK(manifest_find_prefix_collision(&manifest, &collision));
  TEST_CHECK(str_eq(collision.ancestor_path, S("public/Assets/Icon")));
  TEST_CHECK(str_eq(collision.ancestor_label, S("Icon.jpg")));
  TEST_CHECK(str_eq(collision.descendant_path, S("public/assets/icon/image.jpg")));
  TEST_CHECK(str_eq(collision.descendant_label, S("image.jpg")));
}

// The scan examines each path's ancestor prefixes, so it reports the pair whichever order the two
// were added in: the shorter path is always an ancestor of the longer, and here the longer is added
// first, so nothing but the ancestor walk finds it. The shorter path is still named as the
// ancestor.
static void test_detects_prefix_collision_regardless_of_order(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};

  TEST_CHECK(manifest_add(&manifest, S("a/b/c"), S("q.jpg"), &arena, NULL) == 0);
  TEST_CHECK(manifest_add(&manifest, S("a/b"), S("p.jpg"), &arena, NULL) == 0);

  struct ManifestPrefixCollision collision = {0};
  TEST_CHECK(manifest_find_prefix_collision(&manifest, &collision));
  TEST_CHECK(str_eq(collision.ancestor_path, S("a/b")));
  TEST_CHECK(str_eq(collision.descendant_path, S("a/b/c")));
}

// Successive calls report every path that nests under another, once each and paired with its
// shortest recorded ancestor, then report no more.
static void test_detects_every_prefix_collision(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};

  TEST_CHECK(manifest_add(&manifest, S("x/y"), S("p.md"), &arena, NULL) == 0);
  TEST_CHECK(manifest_add(&manifest, S("x/y/z"), S("q.md"), &arena, NULL) == 0);
  TEST_CHECK(manifest_add(&manifest, S("m/n"), S("r.md"), &arena, NULL) == 0);
  TEST_CHECK(manifest_add(&manifest, S("x/y/z/w"), S("s.md"), &arena, NULL) == 0);
  TEST_CHECK(manifest_add(&manifest, S("m/n/o"), S("t.md"), &arena, NULL) == 0);

  static const char* const expected[][2] = {{"x/y/z", "x/y"}, {"x/y/z/w", "x/y"}, {"m/n/o", "m/n"}};
  struct ManifestPrefixCollision collision = {0};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(expected) / sizeof(expected[0])); i++) {
    TEST_CHECK(manifest_find_prefix_collision(&manifest, &collision));
    TEST_CHECK(str_eq(collision.descendant_path, str_from_cstr(expected[i][0])));
    TEST_CHECK(str_eq(collision.ancestor_path, str_from_cstr(expected[i][1])));
    TEST_MSG("collision %td", i);
  }
  TEST_CHECK(!manifest_find_prefix_collision(&manifest, &collision));
}

// Distinct sibling paths that merely share a directory prefix do not collide: `public` and
// `public/trips` are directory components, not recorded outputs, so no recorded path is a prefix of
// another. A leading-run match alone is not enough; the exact-length lookup is what keeps this from
// a false positive.
static void test_no_prefix_collision_among_distinct_files(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};

  TEST_CHECK(manifest_add(&manifest, S("public/index.html"), S("media"), &arena, NULL) == 0);
  TEST_CHECK(manifest_add(&manifest, S("public/trips/index.html"), S("trips"), &arena, NULL) == 0);
  TEST_CHECK(
      manifest_add(&manifest, S("public/map.xml"), S("aggregate_templates[0]"), &arena, NULL) == 0);

  struct ManifestPrefixCollision collision = {0};
  TEST_CHECK(!manifest_find_prefix_collision(&manifest, &collision));
}

TEST_LIST = {
    {"inserts then detects duplicate", test_inserts_then_detects_duplicate},
    {"detects ASCII-folded duplicate", test_detects_ascii_folded_duplicate},
    {"many paths stay findable", test_many_paths_stay_findable},
    {"nul byte is ordinary", test_nul_byte_is_ordinary},
    {"deep trie preserves folded lookup", test_deep_trie_preserves_folded_lookup},
    {"detects prefix collision", test_detects_prefix_collision},
    {"detects ASCII-folded prefix collision", test_detects_ascii_folded_prefix_collision},
    {"detects prefix collision regardless of order",
     test_detects_prefix_collision_regardless_of_order},
    {"detects every prefix collision", test_detects_every_prefix_collision},
    {"no prefix collision among distinct files", test_no_prefix_collision_among_distinct_files},
    {NULL, NULL},
};
