#include "runtime/walk.h"

#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core/path_list.h"
#include "os_memory.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// These tests walk the in-memory host, so each builds its tree in a few calls and nothing is left
// on disk. `test_fs.c` walks a real tree once, for what only the POSIX listing can show.

/**
 * @brief Walks `root_dir` for one case-sensitive suffix, leaving nothing out.
 *
 * @param paths    Path list that receives the matching paths. Must not be `NULL`.
 * @param root_dir Terminated directory tree to walk. Must not be `NULL`.
 * @param suffix   Terminated filename suffix to match. Must not be `NULL`.
 * @param os       Host to walk. Must not be `NULL`.
 * @param log      Log that receives a failure, led by the operation `op`. Must not be `NULL`.
 * @return `0` on success, or `-1` on a directory or entry failure.
 */
static int walk(struct PathList* paths,
                const char* root_dir,
                const char* suffix,
                struct Os* os,
                struct Log* log) {
  static struct Arena perm;
  if (perm.beg == NULL) {
    perm = test_arena();
  }
  const struct Str suffix_str = str_from_cstr(suffix);
  return walk_files_with_suffixes(paths, str_from_cstr(root_dir), S(""), &suffix_str, 1, false, os,
                                  &perm, test_arena(), S("op"), log);
}

/**
 * @brief Reports whether `paths` holds exactly `expected`, in order, printing it when it does not.
 *
 * @param paths          List to compare. Must not be `NULL`.
 * @param expected       Array of `expected_count` terminated paths. Must not be `NULL` when
 *                       `expected_count` is positive.
 * @param expected_count Number of expected paths.
 * @return `true` when the lists match.
 */
static bool is_path_list(const struct PathList* paths,
                         const char* const* expected,
                         ptrdiff_t expected_count) {
  bool is_equal = paths->count == expected_count;
  for (ptrdiff_t i = 0; is_equal && i < expected_count; i++) {
    is_equal = str_eq(paths->items[i], str_from_cstr(expected[i]));
  }
  TEST_CHECK_(is_equal, "walk lists the expected paths");
  for (ptrdiff_t i = 0; !is_equal && i < paths->count; i++) {
    TEST_MSG("listed: '%.*s'", STR_FMT(paths->items[i]));
  }
  return is_equal;
}

// The walk returns only matching files, descends into subdirectories, sorts the result, and records
// nothing on success, whatever order the host lists entries in.
static void test_walk_matches_suffix(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/nested/b.md", "nested");
  os_memory_write(os, "site/a.md", "root");
  os_memory_write(os, "site/nested/c.txt", "ignored");
  os_memory_write(os, "site/Z.md", "upper");

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "site", ".md", os, &log) == 0);
  static const char* const expected[] = {"site/Z.md", "site/a.md", "site/nested/b.md"};
  TEST_CHECK(is_path_list(&paths, expected, 3));
  TEST_CHECK(test_log_is_empty(&log));
}

// An empty suffix matches every regular file in the tree.
static void test_walk_empty_suffix_matches_all(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/a.md", "a");
  os_memory_write(os, "site/b.txt", "b");

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "site", "", os, &log) == 0);
  static const char* const expected[] = {"site/a.md", "site/b.txt"};
  TEST_CHECK(is_path_list(&paths, expected, 2));
}

// A suffix list accepts several extensions, optionally folds ASCII case, and matches nothing when
// it is empty.
static void test_walk_matches_suffix_list_and_case(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/a.md", "a");
  os_memory_write(os, "site/b.MarkDown", "b");
  os_memory_write(os, "site/c.HTML", "c");
  os_memory_write(os, "site/d.txt", "d");

  static const struct Str suffixes[] = {STR_INIT(".md"), STR_INIT(".markdown"), STR_INIT(".html")};
  struct Log log = test_log();
  struct Arena arena = test_arena();
  struct PathList paths = {0};
  TEST_CHECK(walk_files_with_suffixes(&paths, S("site"), S(""), suffixes, 3, true, os, &arena,
                                      test_arena(), S("op"), &log) == 0);
  static const char* const expected[] = {"site/a.md", "site/b.MarkDown", "site/c.HTML"};
  TEST_CHECK(is_path_list(&paths, expected, 3));

  paths = (struct PathList){0};
  TEST_CHECK(walk_files_with_suffixes(&paths, S("site"), S(""), suffixes, 0, true, os, &arena,
                                      test_arena(), S("op"), &log) == 0);
  TEST_CHECK(paths.count == 0);
}

// An empty directory is a successful walk with no matches, not a failure.
static void test_walk_accepts_empty_dir(void) {
  struct Os* os = os_memory();
  os_memory_mkdir(os, "site");

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "site", ".md", os, &log) == 0);
  TEST_CHECK(paths.count == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// A symlinked directory that loops back to the directory holding it, or to an ancestor further out,
// is skipped, so the walk completes and still finds the real files.
static void test_walk_skips_symlink_cycles(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/page.md", "x");
  os_memory_write(os, "site/sub/deeper.md", "x");
  os_memory_symlink(os, ".", "site/loop");
  os_memory_symlink(os, "..", "site/sub/up");

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "site", ".md", os, &log) == 0);
  static const char* const expected[] = {"site/page.md", "site/sub/deeper.md"};
  TEST_CHECK(is_path_list(&paths, expected, 2));
}

// Two sibling symlinks to one directory list its files once, under the directory's own path, even
// though both links sort before it.
static void test_walk_walks_aliased_dir_once(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/real/page.md", "x");
  os_memory_symlink(os, "real", "site/a");
  os_memory_symlink(os, "real", "site/b");

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "site", ".md", os, &log) == 0);
  static const char* const expected[] = {"site/real/page.md"};
  TEST_CHECK(is_path_list(&paths, expected, 1));
}

// A chain of directories outside the root, each reached through two sibling symlinks, lists its one
// file once rather than once per path, which doubles at every level. With no real path to the file
// inside the root, the listed path is the first through the links in byte order.
static void test_walk_walks_alias_chain_once(void) {
  struct Os* os = os_memory();
  os_memory_mkdir(os, "root");
  os_memory_mkdir(os, "l1");
  os_memory_mkdir(os, "l2");
  os_memory_write(os, "l3/page.md", "x");
  // `root/{a,b} -> l1`, `l1/{x,y} -> l2` and `l2/{x,y} -> l3` reach `page.md` by eight paths.
  static const char* const links[][2] = {
      {"../l1", "root/a"}, {"../l1", "root/b"}, {"../l2", "l1/x"},
      {"../l2", "l1/y"},   {"../l3", "l2/x"},   {"../l3", "l2/y"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(links) / sizeof(links[0])); i++) {
    os_memory_symlink(os, links[i][0], links[i][1]);
  }

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "root", ".md", os, &log) == 0);
  static const char* const expected[] = {"root/a/x/x/page.md"};
  TEST_CHECK(is_path_list(&paths, expected, 1));
}

// A dangling symlink and one that resolves in a cycle name no file, so the walk steps past them.
static void test_walk_skips_links_to_nothing(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/page.md", "x");
  os_memory_symlink(os, "missing.md", "site/dangling.md");
  os_memory_symlink(os, "self.md", "site/self.md");

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "site", ".md", os, &log) == 0);
  static const char* const expected[] = {"site/page.md"};
  TEST_CHECK(is_path_list(&paths, expected, 1));
  TEST_CHECK(test_log_is_empty(&log));
}

// A directory passed as `excluded_dir` is left out with everything below it on every path that
// reaches it, its own path and a symlinked alias alike, whichever of those spellings the caller
// passes. A sibling that only shares its name as a prefix is still walked, and an `excluded_dir`
// that does not exist excludes nothing.
static void test_walk_skips_excluded_dir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/page.md", "x");
  os_memory_write(os, "site/public/old.md", "x");
  os_memory_write(os, "site/publicx/page.md", "x");
  os_memory_symlink(os, "public", "site/alias");

  static const struct Str md_suffixes[] = {STR_INIT(".md")};
  static const struct Str exclusions[] = {STR_INIT("site/public"), STR_INIT("site/alias"),
                                          STR_INIT("./site/public/")};
  struct Log log = test_log();
  struct Arena arena = test_arena();
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(exclusions) / sizeof(exclusions[0])); i++) {
    struct PathList paths = {0};
    TEST_CHECK(walk_files_with_suffixes(&paths, S("site"), exclusions[i], md_suffixes, 1, false, os,
                                        &arena, test_arena(), S("op"), &log) == 0);
    static const char* const expected[] = {"site/page.md", "site/publicx/page.md"};
    TEST_CHECK(is_path_list(&paths, expected, 2));
  }

  struct PathList paths = {0};
  TEST_CHECK(walk_files_with_suffixes(&paths, S("site"), S("site/missing"), md_suffixes, 1, false,
                                      os, &arena, test_arena(), S("op"), &log) == 0);
  static const char* const expected[] = {"site/page.md", "site/public/old.md",
                                         "site/publicx/page.md"};
  TEST_CHECK(is_path_list(&paths, expected, 3));
}

// An excluded root is not walked at all, because the root is the first directory the walk visits.
static void test_walk_skips_excluded_root(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/page.md", "x");

  static const struct Str md_suffixes[] = {STR_INIT(".md")};
  struct Log log = test_log();
  struct Arena arena = test_arena();
  struct PathList paths = {0};
  TEST_CHECK(walk_files_with_suffixes(&paths, S("site"), S("site"), md_suffixes, 1, false, os,
                                      &arena, test_arena(), S("op"), &log) == 0);
  TEST_CHECK(paths.count == 0);
}

// A listing allocates for the directory's own entries, so a tree of many directories walks in the
// scratch a test gets, as it does on a real filesystem.
static void test_walk_lists_many_directories(void) {
  enum { DIR_COUNT = 2000 };
  struct Os* os = os_memory();
  for (ptrdiff_t i = 0; i < DIR_COUNT; i++) {
    char file_path[32];
    (void)snprintf(file_path, sizeof(file_path), "site/d%04td/page.md", i);
    os_memory_write(os, file_path, "x");
  }

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "site", ".md", os, &log) == 0);
  TEST_CHECK(paths.count == DIR_COUNT);
  TEST_CHECK(paths.count == DIR_COUNT && str_eq(paths.items[0], S("site/d0000/page.md")) &&
             str_eq(paths.items[DIR_COUNT - 1], S("site/d1999/page.md")));
  TEST_CHECK(test_log_is_empty(&log));
}

// A directory's listing and the paths joined onto it are released once the walk leaves it, so the
// scratch a walk needs grows with the deepest branch of the tree rather than with every file in it.
// The tree's listings together are several times the scratch given here.
static void test_walk_releases_each_directory_listing(void) {
  enum { DIR_COUNT = 40, FILE_COUNT = 40, SCRATCH_LEN = 32 * 1024 };
  struct Os* os = os_memory();
  for (ptrdiff_t i = 0; i < DIR_COUNT; i++) {
    char file_path[48];
    (void)snprintf(file_path, sizeof(file_path), "site/d%02td/page.md", i);
    os_memory_write(os, file_path, "x");
    for (ptrdiff_t j = 0; j < FILE_COUNT; j++) {
      (void)snprintf(file_path, sizeof(file_path), "site/d%02td/image-%02td.png", i, j);
      os_memory_write(os, file_path, "x");
    }
  }

  static const struct Str md_suffixes[] = {STR_INIT(".md")};
  struct Log log = test_log();
  struct Arena perm = test_arena();
  struct Arena region = test_arena();
  const struct Arena scratch = arena_split(&region, SCRATCH_LEN);
  struct PathList paths = {0};
  TEST_CHECK(walk_files_with_suffixes(&paths, S("site"), S(""), md_suffixes, 1, false, os, &perm,
                                      scratch, S("op"), &log) == 0);
  TEST_CHECK(paths.count == DIR_COUNT);
  TEST_CHECK(test_log_is_empty(&log));
}

// A missing root fails and names the root it could not reach, so the caller does not repeat it.
static void test_walk_rejects_missing_dir(void) {
  struct Os* os = os_memory();

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "site", ".md", os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: cannot inspect directory: %s ('site')\n", strerror(ENOENT)));
}

// A regular file as the root is rejected by the directory listing rather than by the identity
// check, which does not look at the type.
static void test_walk_rejects_file_root(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "plain.txt", "x");

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "plain.txt", ".md", os, &log) == -1);
  TEST_CHECK(paths.count == 0);
  TEST_CHECK(test_log_is(&log, "op: cannot open directory: %s ('plain.txt')\n", strerror(ENOTDIR)));
}

// A subdirectory that cannot be listed fails the walk and names that subdirectory, so a tree with a
// part the walk could not see never reports success.
static void test_walk_rejects_unlistable_subdir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/a.md", "x");
  os_memory_write(os, "site/sealed/b.md", "x");
  os_memory_fail(os, OS_MEMORY_OP_LIST, "site/sealed", EACCES);

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "site", ".md", os, &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "op: cannot open directory: %s ('site/sealed')\n", strerror(EACCES)));
}

// An entry that exists but cannot be inspected fails the walk and names that entry, rather than
// being dropped from the listing while the walk still reports success.
static void test_walk_rejects_unstatable_entry(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/sealed/a.md", "hidden");
  os_memory_fail(os, OS_MEMORY_OP_STAT, "site/sealed/a.md", EACCES);

  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "site", ".md", os, &log) == -1);
  TEST_CHECK(paths.count == 0);
  TEST_CHECK(
      test_log_is(&log, "op: cannot inspect entry: %s ('site/sealed/a.md')\n", strerror(EACCES)));
}

// An entry the listing could not inspect is reported when the sorted walk reaches it: of two such
// siblings, the first in byte order, and an error inside an earlier subdirectory before a later
// such sibling. A symlink through a regular file is such an entry. The host lists newest first, so
// each tree is built in the order a host listing in creation order would get wrong.
static void test_walk_reports_uninspectable_entries_in_order(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "file.md", "x");
  os_memory_symlink(os, "../file.md/y", "site/a.md");
  os_memory_symlink(os, "../file.md/y", "site/b.md");
  struct Log log = test_log();
  struct PathList paths = {0};
  TEST_CHECK(walk(&paths, "site", ".md", os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: cannot inspect entry: %s ('site/a.md')\n", strerror(ENOTDIR)));

  os = os_memory();
  os_memory_write(os, "file.md", "x");
  os_memory_symlink(os, "../../file.md/y", "site/a/z.md");
  os_memory_symlink(os, "../file.md/y", "site/b.md");
  log = test_log();
  paths = (struct PathList){0};
  TEST_CHECK(walk(&paths, "site", ".md", os, &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "op: cannot inspect entry: %s ('site/a/z.md')\n", strerror(ENOTDIR)));
}

TEST_LIST = {
    {"walk matches suffix", test_walk_matches_suffix},
    {"walk empty suffix matches all", test_walk_empty_suffix_matches_all},
    {"walk matches suffix list and case", test_walk_matches_suffix_list_and_case},
    {"walk accepts empty dir", test_walk_accepts_empty_dir},
    {"walk skips symlink cycles", test_walk_skips_symlink_cycles},
    {"walk walks aliased dir once", test_walk_walks_aliased_dir_once},
    {"walk walks alias chain once", test_walk_walks_alias_chain_once},
    {"walk skips links to nothing", test_walk_skips_links_to_nothing},
    {"walk skips excluded dir", test_walk_skips_excluded_dir},
    {"walk skips excluded root", test_walk_skips_excluded_root},
    {"walk lists many directories", test_walk_lists_many_directories},
    {"walk releases each directory listing", test_walk_releases_each_directory_listing},
    {"walk rejects missing dir", test_walk_rejects_missing_dir},
    {"walk rejects file root", test_walk_rejects_file_root},
    {"walk rejects unlistable subdir", test_walk_rejects_unlistable_subdir},
    {"walk rejects unstatable entry", test_walk_rejects_unstatable_entry},
    {"walk reports uninspectable entries in order",
     test_walk_reports_uninspectable_entries_in_order},
    {NULL, NULL},
};
