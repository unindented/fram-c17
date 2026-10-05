#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/path.h"
#include "core/path_list.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few bytes. */
enum { TEST_FILE_LEN_MAX = 1024 };

/**
 * @brief Formats the system reason an `fs` action reports.
 *
 * Derives the text from the running libc so exact assertions remain portable.
 *
 * @param buf          Buffer that receives the terminated reason.
 * @param error_number System error number to describe.
 * @return `buf` containing the system message, or an empty `buf` after recording a test-plumbing
 *         failure.
 */
static const char* expected_errno_reason(char buf[static FS_REASON_SIZE], int error_number) {
  const int reason_len = snprintf(buf, FS_REASON_SIZE, "%s", strerror(error_number));
  if (!TEST_CHECK(reason_len > 0 && (size_t)reason_len < FS_REASON_SIZE)) {
    buf[0] = '\0';
  }
  return buf;
}

/**
 * @brief Lists files under `root_dir` matching one case-sensitive suffix.
 *
 * Adapts `fs_list_files_with_suffixes` to the single-suffix shape most tests here need.
 *
 * @param paths      Initialized path list that receives the matching paths. Must not be `NULL`.
 * @param root_dir   Directory tree to walk. Must not be `NULL`.
 * @param suffix     Literal filename suffix to match, such as `.jpg`. Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a directory, entry, or allocation failure.
 */
static int list_files(struct PathList* paths,
                      const char* root_dir,
                      const char* suffix,
                      char* reason,
                      size_t reason_len) {
  const char* suffixes[] = {suffix};
  return fs_list_files_with_suffixes(paths, root_dir, NULL, suffixes, 1, false, reason, reason_len);
}

/** Signals `count_interrupt` has handled since a test last reset it. */
static volatile sig_atomic_t interrupt_count = 0;

/**
 * @brief Counts one delivered signal and does nothing else.
 *
 * Installed without `SA_RESTART`, so each delivery interrupts a blocked system call.
 *
 * @param signal_number Signal delivered. Unused.
 */
static void count_interrupt(int signal_number) {
  (void)signal_number;
  interrupt_count++;
}

/**
 * @brief Opens `fifo_path` for reading, waits, then reads it to end of file.
 *
 * The wait leaves a writer blocked on a full FIFO long enough for signals to interrupt it. This
 * runs in a forked child, so it reports through its return value rather than test assertions.
 *
 * @param fifo_path    FIFO to read. Must not be `NULL`.
 * @param expected     Bytes the writer sends. Must hold at least `expected_len` bytes. Must not be
 *                     `NULL`.
 * @param expected_len Number of bytes the writer sends.
 * @return `0` when exactly `expected` arrived, or `-1` on an open or read failure or other bytes.
 */
static int drain_fifo(const char* fifo_path, const char* expected, size_t expected_len) {
  const int fd = open(fifo_path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  const struct timespec delay = {.tv_nsec = 100 * 1000 * 1000};
  (void)nanosleep(&delay, NULL);
  char buffer[4096];
  size_t total = 0;
  int rc = 0;
  for (;;) {
    const ssize_t nread = read(fd, buffer, sizeof(buffer));
    if (nread == 0) {
      break;
    }
    if (nread < 0 || total + (size_t)nread > expected_len ||
        memcmp(buffer, expected + total, (size_t)nread) != 0) {
      rc = -1;
      break;
    }
    total += (size_t)nread;
  }
  (void)close(fd);
  return rc == 0 && total == expected_len ? 0 : -1;
}

// The `NULL, 0` reason arguments throughout this file are the documented option, not forgotten
// assertions. `fs`'s contract lets `reason` be `NULL` when `reason_len` is 0, and a fixture write
// that fails is a broken test rather than behavior under test. This is the one suite whose subject
// *is* that diagnostic, so the calls that do assert a reason pass a real buffer. No test here takes
// the `NULL`-reason path on a *failing* call. That is deliberate rather than a gap: the pair
// reaches `error_report`, pinned once at that boundary by `test_report_error_accepts_null_buffer`
// instead of once per `fs` action.

// Suffix discovery returns only matching files, walks into subdirectories, and orders results
// deterministically. A successful listing also leaves the reason buffer untouched.
static void test_list_files_matches_suffix(void) {
  char root_dir_template[] = "/tmp/fram-fs-suffix-match.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* nested_jpg = path_join(root_dir, "nested/b.jpg", &arena);
  char* root_jpg = path_join(root_dir, "a.jpg", &arena);
  char* ignored = path_join(root_dir, "nested/c.txt", &arena);
  TEST_CHECK(fs_write_file(nested_jpg, "nested", strlen("nested"), NULL, 0) == 0);
  TEST_CHECK(fs_write_file(root_jpg, "root", strlen("root"), NULL, 0) == 0);
  TEST_CHECK(fs_write_file(ignored, "ignored", strlen("ignored"), NULL, 0) == 0);

  struct PathList paths;
  path_list_init(&paths);
  char reason[FS_REASON_SIZE] = "untouched";
  TEST_CHECK(list_files(&paths, root_dir, ".jpg", reason, sizeof(reason)) == 0);
  TEST_CHECK(paths.count == 2);
  TEST_CHECK(strcmp(paths.items[0], root_jpg) == 0);
  TEST_CHECK(strcmp(paths.items[1], nested_jpg) == 0);
  TEST_CHECK(strcmp(reason, "untouched") == 0);
  path_list_free(&paths);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An empty suffix matches every regular file in the tree.
static void test_list_files_empty_suffix_matches_all(void) {
  char root_dir_template[] = "/tmp/fram-fs-suffix.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* jpg_path = path_join(root_dir, "a.jpg", &arena);
  char* txt_path = path_join(root_dir, "b.txt", &arena);
  TEST_CHECK(fs_write_file(jpg_path, "a", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(txt_path, "b", 1, NULL, 0) == 0);

  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(list_files(&paths, root_dir, "", NULL, 0) == 0);
  TEST_CHECK(paths.count == 2);
  path_list_free(&paths);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A suffix-list walk accepts several media extensions, optionally folds ASCII case, and treats an
// empty list as matching nothing.
static void test_list_files_matches_suffix_list_and_case(void) {
  char root_dir_template[] = "/tmp/fram-fs-suffixes.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* jpg = path_join(root_dir, "a.jpg", &arena);
  char* jpeg = path_join(root_dir, "b.JpEg", &arena);
  char* mp4 = path_join(root_dir, "c.MP4", &arena);
  char* txt = path_join(root_dir, "d.txt", &arena);
  TEST_CHECK(fs_write_file(jpg, "a", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(jpeg, "b", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(mp4, "c", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(txt, "d", 1, NULL, 0) == 0);

  static const char* suffixes[] = {".jpg", ".jpeg", ".mp4"};
  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(fs_list_files_with_suffixes(&paths, root_dir, NULL, suffixes, 3, true, NULL, 0) == 0);
  TEST_CHECK(paths.count == 3);
  TEST_CHECK(strcmp(paths.items[0], jpg) == 0);
  TEST_CHECK(strcmp(paths.items[1], jpeg) == 0);
  TEST_CHECK(strcmp(paths.items[2], mp4) == 0);
  path_list_free(&paths);

  path_list_init(&paths);
  TEST_CHECK(fs_list_files_with_suffixes(&paths, root_dir, NULL, suffixes, 0, true, NULL, 0) == 0);
  TEST_CHECK(paths.count == 0);
  path_list_free(&paths);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An empty directory is a successful listing with no matches, not a failure.
static void test_list_files_accepts_empty_dir(void) {
  char root_dir_template[] = "/tmp/fram-fs-list-empty.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(list_files(&paths, root_dir, ".jpg", NULL, 0) == 0);
  TEST_CHECK(paths.count == 0);
  path_list_free(&paths);

  remove_fixture_tree(root_dir);
}

// A symlinked directory that loops back into its own ancestry is skipped, so the walk completes and
// still finds the real files instead of recursing until the path overflows.
static void test_list_files_skips_symlink_cycle(void) {
  char root_dir_template[] = "/tmp/fram-fs-list.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* photo = path_join(root_dir, "photo.jpg", &arena);
  char* loop = path_join(root_dir, "loop", &arena);
  TEST_CHECK(fs_write_file(photo, "x", 1, NULL, 0) == 0);
  // `loop -> .` resolves to its own directory, so descending into it is a cycle.
  if (!TEST_CHECK(symlink(".", loop) == 0)) {
    goto cleanup;
  }

  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(list_files(&paths, root_dir, ".jpg", NULL, 0) == 0);
  TEST_CHECK(paths.count == 1);
  TEST_CHECK(strcmp(paths.items[0], photo) == 0);
  path_list_free(&paths);

cleanup:
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A symlinked directory whose target is an outer ancestor, rather than the directory holding it, is
// skipped too, because the walk already visited that ancestor.
static void test_list_files_skips_symlink_cycle_to_ancestor(void) {
  char root_dir_template[] = "/tmp/fram-fs-list-ancestor.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* photo = path_join(root_dir, "sub/photo.jpg", &arena);
  char* up = path_join(root_dir, "sub/up", &arena);
  TEST_CHECK(fs_write_file(photo, "x", 1, NULL, 0) == 0);
  // `sub/up -> ..` resolves to the walk's root, which is `sub`'s parent rather than `sub` itself,
  // so the cycle is one crumb further out than the directory being scanned.
  if (!TEST_CHECK(symlink("..", up) == 0)) {
    goto cleanup;
  }

  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(list_files(&paths, root_dir, ".jpg", NULL, 0) == 0);
  TEST_CHECK(paths.count == 1);
  TEST_CHECK(strcmp(paths.items[0], photo) == 0);
  path_list_free(&paths);

cleanup:
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Two sibling symlinks to one directory list its files once, under the directory's own path, even
// though both links sort before it. Walking each alias would publish a copy of every file per path.
static void test_list_files_walks_aliased_dir_once(void) {
  char root_dir_template[] = "/tmp/fram-fs-list-alias.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* photo = path_join(root_dir, "real/photo.jpg", &arena);
  char* alias_a = path_join(root_dir, "a", &arena);
  char* alias_b = path_join(root_dir, "b", &arena);
  TEST_CHECK(fs_write_file(photo, "x", 1, NULL, 0) == 0);
  if (!TEST_CHECK(symlink("real", alias_a) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(symlink("real", alias_b) == 0)) {
    goto cleanup;
  }

  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(list_files(&paths, root_dir, ".jpg", NULL, 0) == 0);
  TEST_CHECK(paths.count == 1);
  TEST_CHECK(paths.count == 1 && strcmp(paths.items[0], photo) == 0);
  path_list_free(&paths);

cleanup:
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A chain of directories outside the root, each reached through two sibling symlinks, lists its one
// file once rather than once per path, which doubles at every level. With no real path to the file
// inside the root, the listed path is the first through the links in byte order.
static void test_list_files_walks_alias_chain_once(void) {
  char root_dir_template[] = "/tmp/fram-fs-list-chain.XXXXXX";
  const char* base_dir = init_fixture_dir(root_dir_template);
  if (base_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* root_dir = path_join(base_dir, "root", &arena);
  char* level_photo = path_join(base_dir, "l3/photo.jpg", &arena);
  char* expected = path_join(root_dir, "a/x/x/photo.jpg", &arena);
  TEST_CHECK(fs_mkdir_p(root_dir, NULL, 0) == 0);
  TEST_CHECK(fs_mkdir_p(path_join(base_dir, "l1", &arena), NULL, 0) == 0);
  TEST_CHECK(fs_mkdir_p(path_join(base_dir, "l2", &arena), NULL, 0) == 0);
  TEST_CHECK(fs_write_file(level_photo, "x", 1, NULL, 0) == 0);
  // `root/{a,b} -> l1`, `l1/{x,y} -> l2` and `l2/{x,y} -> l3` reach `photo.jpg` by eight paths.
  const char* const links[][2] = {
      {"../l1", "root/a"}, {"../l1", "root/b"}, {"../l2", "l1/x"},
      {"../l2", "l1/y"},   {"../l3", "l2/x"},   {"../l3", "l2/y"},
  };
  for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
    if (!TEST_CHECK(symlink(links[i][0], path_join(base_dir, links[i][1], &arena)) == 0)) {
      goto cleanup;
    }
  }

  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(list_files(&paths, root_dir, ".jpg", NULL, 0) == 0);
  TEST_CHECK(paths.count == 1);
  TEST_CHECK(paths.count == 1 && strcmp(paths.items[0], expected) == 0);
  path_list_free(&paths);

cleanup:
  arena_free(&arena);
  remove_fixture_tree(base_dir);
}

// A directory passed as `excluded_dir` is left out with everything below it on every path that
// reaches it, its own path and a symlinked alias alike, because the walk compares identities. The
// exclusion names the same directory whichever of those spellings the caller passes. A sibling that
// only shares its name as a prefix is still walked, and an `excluded_dir` that does not exist
// excludes nothing.
static void test_list_files_skips_excluded_dir(void) {
  char root_dir_template[] = "/tmp/fram-fs-list-excluded.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* photo = path_join(root_dir, "photo.jpg", &arena);
  char* excluded_photo = path_join(root_dir, "public/old.jpg", &arena);
  char* sibling_photo = path_join(root_dir, "publicx/photo.jpg", &arena);
  char* excluded_dir = path_join(root_dir, "public", &arena);
  char* alias_dir = path_join(root_dir, "alias", &arena);
  char* missing_dir = path_join(root_dir, "missing", &arena);
  TEST_CHECK(fs_write_file(photo, "x", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(excluded_photo, "x", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(sibling_photo, "x", 1, NULL, 0) == 0);
  const char* const jpg_suffixes[] = {".jpg"};
  const char* const exclusions[] = {excluded_dir, alias_dir};
  if (!TEST_CHECK(symlink("public", alias_dir) == 0)) {
    goto cleanup;
  }

  for (size_t i = 0; i < sizeof(exclusions) / sizeof(exclusions[0]); i++) {
    struct PathList paths;
    path_list_init(&paths);
    TEST_CHECK(fs_list_files_with_suffixes(&paths, root_dir, exclusions[i], jpg_suffixes, 1, false,
                                           NULL, 0) == 0);
    TEST_CHECK(paths.count == 2);
    TEST_CHECK(paths.count == 2 && strcmp(paths.items[0], photo) == 0 &&
               strcmp(paths.items[1], sibling_photo) == 0);
    path_list_free(&paths);
  }

  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(fs_list_files_with_suffixes(&paths, root_dir, missing_dir, jpg_suffixes, 1, false,
                                         NULL, 0) == 0);
  TEST_CHECK(paths.count == 3);
  TEST_CHECK(paths.count == 3 && strcmp(paths.items[0], photo) == 0 &&
             strcmp(paths.items[1], excluded_photo) == 0 &&
             strcmp(paths.items[2], sibling_photo) == 0);
  path_list_free(&paths);

cleanup:
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A tree deeper than the open-file limit is walked, because the walk closes each directory before
// descending into its subdirectories rather than holding one stream open per level.
static void test_list_files_walks_tree_deeper_than_open_file_limit(void) {
  enum { TREE_DEPTH = 64, OPEN_FILE_LIMIT = 32 };
  char root_dir_template[] = "/tmp/fram-fs-list-deep.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* leaf_dir = path_join(root_dir, "d", &arena);
  for (size_t i = 1; i < TREE_DEPTH; i++) {
    leaf_dir = path_join(leaf_dir, "d", &arena);
  }
  char* photo = path_join(leaf_dir, "photo.jpg", &arena);
  TEST_CHECK(fs_write_file(photo, "x", 1, NULL, 0) == 0);

  struct rlimit limit;
  struct rlimit lowered;
  struct PathList paths;
  path_list_init(&paths);
  char reason[FS_REASON_SIZE] = "";
  int rc = -1;
  if (!TEST_CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0)) {
    goto cleanup;
  }
  lowered = (struct rlimit){.rlim_cur = OPEN_FILE_LIMIT, .rlim_max = limit.rlim_max};
  if (!TEST_CHECK(setrlimit(RLIMIT_NOFILE, &lowered) == 0)) {
    goto cleanup;
  }
  rc = list_files(&paths, root_dir, ".jpg", reason, sizeof(reason));
  // Restore the limit before asserting, so a failure cannot starve later tests of descriptors.
  (void)setrlimit(RLIMIT_NOFILE, &limit);

  TEST_CHECK(rc == 0);
  TEST_MSG("reason: %s", reason);
  TEST_CHECK(paths.count == 1 && strcmp(paths.items[0], photo) == 0);

cleanup:
  path_list_free(&paths);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A missing directory fails and names the root it could not reach, so the caller does not have to
// repeat it.
static void test_list_files_rejects_missing_dir(void) {
  char root_dir_template[] = "/tmp/fram-fs-list-missing.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* missing = path_join(root_dir, "does-not-exist", &arena);
  struct PathList missing_paths;
  path_list_init(&missing_paths);
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(list_files(&missing_paths, missing, ".jpg", reason, sizeof(reason)) == -1);
  // Every walk failure names its own path, so the reason is self-contained.
  char expected[FS_REASON_SIZE];
  char message[FS_REASON_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "cannot inspect directory: %s ('%s')",
               expected_errno_reason(message, ENOENT), missing);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(reason, expected) == 0);

cleanup:
  path_list_free(&missing_paths);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A regular file passed as the root is rejected by the directory open rather than by the `stat`
// that seeds the ancestor chain, which deliberately does not check the type. The directory open
// prevents a walk when an input root names a file instead of a tree.
static void test_list_files_rejects_file_root(void) {
  char root_dir_template[] = "/tmp/fram-fs-list-file-root.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* plain = path_join(root_dir, "plain.txt", &arena);
  TEST_CHECK(fs_write_file(plain, "x", 1, NULL, 0) == 0);

  struct PathList paths;
  path_list_init(&paths);
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(list_files(&paths, plain, ".jpg", reason, sizeof(reason)) == -1);
  TEST_CHECK(paths.count == 0);
  char expected[FS_REASON_SIZE];
  char message[FS_REASON_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected), "cannot open directory: %s ('%s')",
                                    expected_errno_reason(message, ENOTDIR), plain);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(reason, expected) == 0);

cleanup:
  path_list_free(&paths);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An entry that exists but cannot be inspected fails the walk and names that entry, rather than
// being dropped from the listing while the walk still reports success. A directory that is readable
// but not searchable is the reachable way to produce it: `readdir` lists the entry, and `stat` on
// the entry then fails with `EACCES`.
static void test_list_files_rejects_unstatable_entry(void) {
  // Root bypasses the permission bits, so the sealed directory would be walked like any other and
  // the assertions below would fail for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }

  char root_dir_template[] = "/tmp/fram-fs-list-unstatable.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* sealed_dir = path_join(root_dir, "sealed", &arena);
  char* sealed_jpg = path_join(root_dir, "sealed/a.jpg", &arena);
  TEST_CHECK(fs_write_file(sealed_jpg, "hidden", strlen("hidden"), NULL, 0) == 0);
  TEST_CHECK(chmod(sealed_dir, 0400) == 0);

  struct PathList paths;
  path_list_init(&paths);
  char reason[FS_REASON_SIZE] = "";
  const int rc = list_files(&paths, root_dir, ".jpg", reason, sizeof(reason));
  // Restore the mode before checking anything: neither the cleanup below nor the harness can remove
  // a directory it is not allowed to search.
  (void)chmod(sealed_dir, 0700);

  TEST_CHECK(rc == -1);
  TEST_CHECK(paths.count == 0);
  char expected_entry[FS_REASON_SIZE];
  char message[FS_REASON_SIZE];
  const int expected_len =
      snprintf(expected_entry, sizeof(expected_entry), "cannot inspect entry: %s ('%s')",
               expected_errno_reason(message, EACCES), sealed_jpg);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected_entry))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(reason, expected_entry) == 0);

cleanup:
  path_list_free(&paths);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An empty file reads back as zero bytes with a valid terminator.
static void test_read_file_accepts_empty(void) {
  char root_dir_template[] = "/tmp/fram-fs-empty.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* empty_path = path_join(root_dir, "empty.jpg", &arena);
  TEST_CHECK(fs_write_file(empty_path, "", 0, NULL, 0) == 0);

  unsigned char* file_data = NULL;
  size_t file_len = 123;
  TEST_CHECK(fs_read_file(empty_path, TEST_FILE_LEN_MAX, &file_data, &file_len, NULL, 0) == 0);
  TEST_CHECK(file_len == 0);
  TEST_CHECK(file_data != NULL && file_data[0] == '\0');
  free(file_data);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A file exactly at `data_len_max` bytes reads in full. The limit is inclusive.
static void test_read_file_accepts_file_at_limit(void) {
  char root_dir_template[] = "/tmp/fram-fs-read-limit.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* file_path = path_join(root_dir, "limit.jpg", &arena);
  TEST_CHECK(fs_write_file(file_path, "four", strlen("four"), NULL, 0) == 0);

  unsigned char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, strlen("four"), &file_data, &file_len, NULL, 0) == 0);
  TEST_CHECK(file_len == strlen("four"));
  TEST_CHECK(file_data != NULL && strcmp((const char*)file_data, "four") == 0);
  free(file_data);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A binary file carrying an embedded `NUL` reads back byte-for-byte. Text policy belongs to the
// consumer, so the filesystem boundary must not reject valid image bytes.
static void test_read_file_accepts_embedded_nul(void) {
  char root_dir_template[] = "/tmp/fram-fs-nul.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* nul_path = path_join(root_dir, "nul.jpg", &arena);
  TEST_CHECK(fs_write_file(nul_path, "before\0after", sizeof("before\0after") - 1, NULL, 0) == 0);

  unsigned char* file_data = NULL;
  size_t file_len = 999;
  TEST_CHECK(fs_read_file(nul_path, TEST_FILE_LEN_MAX, &file_data, &file_len, NULL, 0) == 0);
  TEST_CHECK(file_len == sizeof("before\0after") - 1);
  TEST_CHECK(memcmp(file_data, "before\0after", file_len) == 0);
  TEST_CHECK(file_data[file_len] == '\0');
  free(file_data);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `fs_read_file` rejects a missing path and a directory, leaving outputs untouched, and reports the
// two as different reasons rather than one indistinguishable failure.
static void test_read_file_rejects_missing_and_non_regular(void) {
  char root_dir_template[] = "/tmp/fram-fs-read.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* missing = path_join(root_dir, "missing.jpg", &arena);

  unsigned char sentinel[] = "unchanged";
  unsigned char* file_data = sentinel;
  size_t file_len = 999;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_read_file(missing, TEST_FILE_LEN_MAX, &file_data, &file_len, reason,
                          sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  char expected[FS_REASON_SIZE];
  TEST_CHECK(strcmp(reason, expected_errno_reason(expected, ENOENT)) == 0);

  // A directory is not a regular file. This is a first-party rejection, not a system error, so it
  // carries its own wording.
  file_data = sentinel;
  file_len = 999;
  reason[0] = '\0';
  TEST_CHECK(fs_read_file(root_dir, TEST_FILE_LEN_MAX, &file_data, &file_len, reason,
                          sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  TEST_CHECK(strcmp(reason, "not a regular file") == 0);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A FIFO is rejected as not a regular file instead of blocking the open until a writer opens it,
// leaving the outputs untouched.
static void test_read_file_rejects_fifo(void) {
  char root_dir_template[] = "/tmp/fram-fs-read-fifo.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* fifo = path_join(root_dir, "fifo.jpg", &arena);
  unsigned char sentinel[] = "unchanged";
  unsigned char* file_data = sentinel;
  size_t file_len = 999;
  char reason[FS_REASON_SIZE] = "";
  if (!TEST_CHECK(mkfifo(fifo, 0600) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(fs_read_file(fifo, TEST_FILE_LEN_MAX, &file_data, &file_len, reason, sizeof(reason)) ==
             -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  TEST_CHECK(strcmp(reason, "not a regular file") == 0);

cleanup:
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A readable file over `data_len_max` is rejected from its `fstat` size, before anything is
// allocated or read, naming the limit and the size and leaving the outputs untouched. The file is
// sized with `truncate`, so its size is the only thing about it the check can see.
static void test_read_file_rejects_oversize_before_reading(void) {
  char root_dir_template[] = "/tmp/fram-fs-read-oversize.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* file_path = path_join(root_dir, "big.jpg", &arena);
  TEST_CHECK(fs_write_file(file_path, "", 0, NULL, 0) == 0);
  TEST_CHECK(truncate(file_path, (off_t)strlen("five") + 1) == 0);

  unsigned char sentinel[] = "unchanged";
  unsigned char* file_data = sentinel;
  size_t file_len = 999;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(
      fs_read_file(file_path, strlen("five"), &file_data, &file_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  TEST_CHECK(strcmp(reason, "exceeds max file size (4 bytes) at 5 bytes") == 0);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A written file reads back byte-for-byte, and a successful write and a successful read each leave
// the reason untouched.
static void test_write_then_read_round_trips(void) {
  char root_dir_template[] = "/tmp/fram-fs-roundtrip.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* root_jpg = path_join(root_dir, "a.jpg", &arena);
  char reason[FS_REASON_SIZE] = "untouched";
  TEST_CHECK(fs_write_file(root_jpg, "root", strlen("root"), reason, sizeof(reason)) == 0);
  TEST_CHECK(strcmp(reason, "untouched") == 0);

  unsigned char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(root_jpg, TEST_FILE_LEN_MAX, &file_data, &file_len, reason,
                          sizeof(reason)) == 0);
  TEST_CHECK(file_len == strlen("root"));
  TEST_CHECK(file_data != NULL && strcmp((const char*)file_data, "root") == 0);
  TEST_CHECK(strcmp(reason, "untouched") == 0);
  free(file_data);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A second write replaces a prior file's contents with arbitrary binary bytes, including bytes past
// the new end.
static void test_write_file_replaces_contents(void) {
  char root_dir_template[] = "/tmp/fram-fs-replace.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* file_path = path_join(root_dir, "nested/output.jpg", &arena);
  TEST_CHECK(fs_write_file(file_path, "old contents", strlen("old contents"), NULL, 0) == 0);
  static const unsigned char replacement[] = {'n', '\0', 'w'};
  TEST_CHECK(fs_write_file(file_path, replacement, sizeof(replacement), NULL, 0) == 0);

  unsigned char* data = NULL;
  size_t data_len = 0;
  TEST_CHECK(fs_read_file(file_path, TEST_FILE_LEN_MAX, &data, &data_len, NULL, 0) == 0);
  TEST_CHECK(data_len == sizeof(replacement));
  TEST_CHECK(data != NULL && memcmp(data, replacement, sizeof(replacement)) == 0);
  free(data);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Every writer creates a new file with `0666` reduced by the process umask, so which writer
// produced an output does not change its permissions and a restrictive umask is not discarded.
// Overwriting an existing file keeps that file's mode, whichever writer does it.
static void test_writers_apply_umask_and_keep_existing_mode(void) {
  char root_dir_template[] = "/tmp/fram-fs-mode.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* plain = path_join(root_dir, "plain.html", &arena);
  char* written = path_join(root_dir, "written.html", &arena);
  char* source = path_join(root_dir, "source.css", &arena);
  char* copied = path_join(root_dir, "copied.css", &arena);
  char* existing_written = path_join(root_dir, "existing.html", &arena);
  char* existing_copied = path_join(root_dir, "existing.css", &arena);

  // A restrictive umask is the case a hardcoded mode would discard. Restore it before asserting, so
  // a failure cannot leak the tightened value into later tests.
  const mode_t previous_umask = umask(077);
  const mode_t expected = (mode_t)(0666 & ~077);

  const unsigned char bytes[] = {'a', 0x00, 'b'};
  TEST_CHECK(fs_write_file(plain, "x", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(written, bytes, sizeof(bytes), NULL, 0) == 0);
  // The source is given wide permissions the copy must not inherit.
  TEST_CHECK(fs_write_file(source, "body{}", 6, NULL, 0) == 0);
  TEST_CHECK(chmod(source, 0777) == 0);
  TEST_CHECK(fs_copy_file(source, copied, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(existing_written, "x", 1, NULL, 0) == 0);
  TEST_CHECK(chmod(existing_written, 0640) == 0);
  TEST_CHECK(fs_write_file(existing_written, "y", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(existing_copied, "x", 1, NULL, 0) == 0);
  TEST_CHECK(chmod(existing_copied, 0640) == 0);
  TEST_CHECK(fs_copy_file(source, existing_copied, NULL, 0) == 0);
  (void)umask(previous_umask);

  struct stat st;
  TEST_CHECK(stat(plain, &st) == 0 && (st.st_mode & 07777) == expected);
  TEST_CHECK(stat(written, &st) == 0 && (st.st_mode & 07777) == expected);
  TEST_CHECK(stat(copied, &st) == 0 && (st.st_mode & 07777) == expected);
  TEST_CHECK(stat(existing_written, &st) == 0 && (st.st_mode & 07777) == 0640);
  TEST_CHECK(stat(existing_copied, &st) == 0 && (st.st_mode & 07777) == 0640);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `fs_write_file` finishes a write that signals interrupt instead of failing with `EINTR`. A FIFO
// whose reader starts late stands in for a slow file, because a write to a local regular file is
// not interrupted. The handler is installed without `SA_RESTART`, so each signal does interrupt the
// blocked `write`.
static void test_write_file_retries_interrupted_write(void) {
  char root_dir_template[] = "/tmp/fram-fs-write-eintr.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* fifo = path_join(root_dir, "fifo.html", &arena);
  // Larger than any pipe buffer, so the write blocks until the reader drains it.
  static char data[1024 * 1024];
  int hold_fd = -1;
  pid_t reader = -1;
  struct sigaction action = {.sa_handler = count_interrupt};
  struct sigaction previous_action;
  const struct itimerval every_millisecond = {.it_interval = {.tv_usec = 1000},
                                              .it_value = {.tv_usec = 1000}};
  const struct itimerval stopped = {0};
  char reason[FS_REASON_SIZE] = "untouched";
  int rc = -1;
  int status = 0;
  if (!TEST_CHECK(mkfifo(fifo, 0600) == 0)) {
    goto cleanup;
  }
  // A read end held open lets the write's `open` return at once, so only its `write` blocks.
  hold_fd = open(fifo, O_RDONLY | O_NONBLOCK);
  if (!TEST_CHECK(hold_fd >= 0)) {
    goto cleanup;
  }

  for (size_t i = 0; i < sizeof(data); i++) {
    data[i] = (char)('a' + i % 26);
  }

  reader = fork();
  if (!TEST_CHECK(reader >= 0)) {
    goto cleanup;
  }
  if (reader == 0) {
    _exit(drain_fifo(fifo, data, sizeof(data)) == 0 ? 0 : 1);
  }

  (void)sigemptyset(&action.sa_mask);
  if (!TEST_CHECK(sigaction(SIGALRM, &action, &previous_action) == 0)) {
    goto cleanup;
  }
  interrupt_count = 0;
  if (!TEST_CHECK(setitimer(ITIMER_REAL, &every_millisecond, NULL) == 0)) {
    (void)sigaction(SIGALRM, &previous_action, NULL);
    goto cleanup;
  }
  rc = fs_write_file(fifo, data, sizeof(data), reason, sizeof(reason));
  // Stop the timer before restoring the previous action, so no signal reaches the default one.
  (void)setitimer(ITIMER_REAL, &stopped, NULL);
  (void)sigaction(SIGALRM, &previous_action, NULL);
  TEST_CHECK(waitpid(reader, &status, 0) == reader);
  reader = -1;

  TEST_CHECK(rc == 0);
  TEST_CHECK(strcmp(reason, "untouched") == 0);
  TEST_CHECK(interrupt_count > 0);
  TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

cleanup:
  // A reader that never saw a writer would block in its `open` forever, so it is stopped rather
  // than awaited.
  if (reader > 0) {
    (void)kill(reader, SIGKILL);
    (void)waitpid(reader, NULL, 0);
  }
  if (hold_fd >= 0) {
    (void)close(hold_fd);
  }
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `fs_write_file` fails when the parent directory cannot be created and when the target is itself a
// directory, and reports the two as different reasons. The first case is what pins that the
// parent-directory failure is propagated rather than discarded. The return value alone cannot show
// that, because a discarded failure would still reach the open, which fails with `ENOTDIR` and
// returns `-1` as well. That is why the assertion is on the reason.
static void test_write_file_rejects_file_parent_and_dir_target(void) {
  char root_dir_template[] = "/tmp/fram-fs-write-fail.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);

  // `plain.txt` is a regular file, so it cannot hold the child the write asks for. The reason comes
  // from the parent-directory pass and names the component that blocked it.
  char* blocking_file = path_join(root_dir, "plain.txt", &arena);
  char* through_file = path_join(root_dir, "plain.txt/child.html", &arena);
  char* dir_target = path_join(root_dir, "sub", &arena);
  TEST_CHECK(fs_write_file(blocking_file, "x", 1, NULL, 0) == 0);
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_write_file(through_file, "x", 1, reason, sizeof(reason)) == -1);
  char expected[FS_REASON_SIZE];
  const int blocking_expected_len =
      snprintf(expected, sizeof(expected), "exists and is not a directory ('%s')", blocking_file);
  if (!TEST_CHECK(blocking_expected_len > 0 && (size_t)blocking_expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(reason, expected) == 0);

  // An existing directory as the target has a parent that is already there, so the walk gets past
  // it and the open is what refuses. That is a system error, so it carries the system message.
  TEST_CHECK(fs_mkdir_p(dir_target, NULL, 0) == 0);
  reason[0] = '\0';
  TEST_CHECK(fs_write_file(dir_target, "x", 1, reason, sizeof(reason)) == -1);
  char message[FS_REASON_SIZE];
  TEST_CHECK(strcmp(reason, expected_errno_reason(message, EISDIR)) == 0);

cleanup:
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Copying streams arbitrary bytes, including `NUL`, into a destination whose parent directories it
// creates. A missing source fails before the destination is opened, so it does not truncate or
// replace an existing destination.
static void test_copy_file_streams_binary_bytes(void) {
  char root_dir_template[] = "/tmp/fram-fs-copy.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* source = path_join(root_dir, "source.mp4", &arena);
  char* dest = path_join(root_dir, "deep/copy.mp4", &arena);
  static const char bytes[] = "video\0bytes";
  TEST_CHECK(fs_write_file(source, bytes, sizeof(bytes) - 1, NULL, 0) == 0);
  TEST_CHECK(fs_copy_file(source, dest, NULL, 0) == 0);

  unsigned char* data = NULL;
  size_t data_len = 0;
  TEST_CHECK(fs_read_file(dest, TEST_FILE_LEN_MAX, &data, &data_len, NULL, 0) == 0);
  TEST_CHECK(data_len == sizeof(bytes) - 1);
  TEST_CHECK(memcmp(data, bytes, data_len) == 0);
  free(data);

  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_copy_file(path_join(root_dir, "missing", &arena), dest, reason, sizeof(reason)) ==
             -1);
  char message[FS_REASON_SIZE];
  TEST_CHECK(strcmp(reason, expected_errno_reason(message, ENOENT)) == 0);
  TEST_CHECK(fs_read_file(dest, TEST_FILE_LEN_MAX, &data, &data_len, NULL, 0) == 0);
  TEST_CHECK(data_len == sizeof(bytes) - 1);
  TEST_CHECK(memcmp(data, bytes, data_len) == 0);
  free(data);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A FIFO source is rejected as not a regular file instead of blocking the copy until a writer opens
// it, and the destination is never created.
static void test_copy_file_rejects_fifo_source(void) {
  char root_dir_template[] = "/tmp/fram-fs-copy-fifo.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* fifo = path_join(root_dir, "source.mp4", &arena);
  char* dest = path_join(root_dir, "copy.mp4", &arena);
  char reason[FS_REASON_SIZE] = "";
  if (!TEST_CHECK(mkfifo(fifo, 0600) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(fs_copy_file(fifo, dest, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason, "not a regular file") == 0);
  TEST_CHECK(!fs_path_exists(dest));

cleanup:
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Metadata reports the exact byte size and a normalized nanosecond component, while rejecting a
// directory without publishing over the caller's sentinel.
static void test_stat_meta_reports_regular_file(void) {
  char root_dir_template[] = "/tmp/fram-fs-meta.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* file_path = path_join(root_dir, "image.jpg", &arena);
  TEST_CHECK(fs_write_file(file_path, "12345", 5, NULL, 0) == 0);
  struct FsMeta meta = {0};
  TEST_CHECK(fs_stat_meta(file_path, &meta, NULL, 0) == 0);
  TEST_CHECK(meta.size_len == 5);
  TEST_CHECK(meta.mtime_s > 0);
  TEST_CHECK(meta.mtime_ns >= 0 && meta.mtime_ns < 1000000000);

  struct FsMeta sentinel = {.mtime_s = 7, .mtime_ns = 11, .size_len = 13};
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_stat_meta(root_dir, &sentinel, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason, "not a regular file") == 0);
  TEST_CHECK(sentinel.mtime_s == 7 && sentinel.mtime_ns == 11 && sentinel.size_len == 13);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `fs_mkdir_p` creates a deep path, treats an empty path as a no-op, and is idempotent.
static void test_mkdir_p_creates_nested_and_is_idempotent(void) {
  char root_dir_template[] = "/tmp/fram-fs-mkdir.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);

  TEST_CHECK(fs_mkdir_p("", NULL, 0) == 0);

  char* nested = path_join(root_dir, "x/y/z", &arena);
  TEST_CHECK(fs_mkdir_p(nested, NULL, 0) == 0);
  struct stat st;
  TEST_CHECK(stat(nested, &st) == 0 && S_ISDIR(st.st_mode));

  // Re-creating an existing tree is not an error.
  TEST_CHECK(fs_mkdir_p(nested, NULL, 0) == 0);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `fs_mkdir_p` fails when a path component is an existing regular file. The reason names the
// component rather than the whole path the caller asked for.
static void test_mkdir_p_rejects_file_component(void) {
  char root_dir_template[] = "/tmp/fram-fs-mkdir-fail.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);

  // `file` is a regular file, so it cannot be the target of `fs_mkdir_p`.
  char* existing_file = path_join(root_dir, "file", &arena);
  char* path_through_file = path_join(root_dir, "file/child", &arena);
  TEST_CHECK(fs_write_file(existing_file, "x", 1, NULL, 0) == 0);
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_mkdir_p(existing_file, reason, sizeof(reason)) == -1);
  char expected[FS_REASON_SIZE];
  const int file_expected_len =
      snprintf(expected, sizeof(expected), "exists and is not a directory ('%s')", existing_file);
  if (!TEST_CHECK(file_expected_len > 0 && (size_t)file_expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(reason, expected) == 0);

  // `file` is a regular file, so it cannot serve as an intermediate path component. The walk stops
  // at that component, so the reason names it rather than the deeper path the caller asked for.
  reason[0] = '\0';
  TEST_CHECK(fs_mkdir_p(path_through_file, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason, expected) == 0);

cleanup:
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `fs_mkdir_p` fails when a component exists but cannot be inspected, which is a distinct branch
// from the regular-file rejection above. `mkdir` reports `EEXIST`, so the walk falls through to a
// `stat` that then fails on its own. A dangling symlink is the reachable way to get there. The link
// itself exists, so `mkdir` refuses, and resolving it fails with `ENOENT`.
static void test_mkdir_p_rejects_unstatable_component(void) {
  char root_dir_template[] = "/tmp/fram-fs-mkdir-unstatable.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);

  char* dangling = path_join(root_dir, "dangling", &arena);
  TEST_CHECK(symlink("/fram-nonexistent-symlink-target", dangling) == 0);

  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_mkdir_p(dangling, reason, sizeof(reason)) == -1);
  char expected[FS_REASON_SIZE];
  char message[FS_REASON_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "cannot inspect directory: %s ('%s')",
               expected_errno_reason(message, ENOENT), dangling);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(reason, expected) == 0);

cleanup:
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `fs_mkdir_p` fails when `mkdir` itself is refused, a third branch again. The failure is neither
// the first-party type rejection nor the `EEXIST` fallthrough, but the system error reported
// straight from the create. A parent that is readable but not writable is the way to produce it.
static void test_mkdir_p_rejects_uncreatable_component(void) {
  // Root bypasses the permission bits, so `mkdir` would succeed and the assertions below would fail
  // for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }

  char root_dir_template[] = "/tmp/fram-fs-mkdir-refused.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* sealed_dir = path_join(root_dir, "sealed", &arena);
  char* child_dir = path_join(root_dir, "sealed/child", &arena);
  TEST_CHECK(fs_mkdir_p(sealed_dir, NULL, 0) == 0);
  TEST_CHECK(chmod(sealed_dir, 0500) == 0);

  char reason[FS_REASON_SIZE] = "";
  const int rc = fs_mkdir_p(child_dir, reason, sizeof(reason));
  // Restore the mode before checking anything, as `test_list_files_rejects_unstatable_entry` does.
  // The cleanup below cannot remove a directory it may not write.
  (void)chmod(sealed_dir, 0700);

  TEST_CHECK(rc == -1);
  char expected[FS_REASON_SIZE];
  char message[FS_REASON_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "cannot create directory: %s ('%s')",
               expected_errno_reason(message, EACCES), child_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(reason, expected) == 0);

cleanup:
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `fs_path_exists` separates an absent path from one that is present but the wrong type, which is
// what lets an optional read root be skipped without skipping a misconfigured one.
static void test_path_exists_separates_absent_from_wrong_type(void) {
  char root_dir_template[] = "/tmp/fram-fs-exists.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* present_dir = path_join(root_dir, "assets", &arena);
  char* present_file = path_join(root_dir, "assets.txt", &arena);
  char* absent = path_join(root_dir, "missing", &arena);
  char* dangling = path_join(root_dir, "dangling", &arena);
  TEST_CHECK(fs_mkdir_p(present_dir, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(present_file, "x", 1, NULL, 0) == 0);
  if (!TEST_CHECK(symlink(absent, dangling) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(fs_path_exists(present_dir));
  // A regular file exists. The caller is expected to let a later operation name the type mismatch.
  TEST_CHECK(fs_path_exists(present_file));
  TEST_CHECK(!fs_path_exists(absent));
  // `stat` follows the link, so a dangling symlink is indistinguishable from an absent path.
  TEST_CHECK(!fs_path_exists(dangling));

cleanup:
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `fs_require_dir` is the reporting counterpart to `fs_path_exists`: it answers the same question
// with a cause, and tells an absent directory apart from one of the wrong type. The reason is the
// bare cause with the path trailing, so a caller can compose it after naming its own operation.
static void test_require_dir_reports_why_a_directory_is_unusable(void) {
  char root_dir_template[] = "/tmp/fram-fs-require-dir.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* present = path_join(root_dir, "assets", &arena);
  char* plain_file = path_join(root_dir, "assets.txt", &arena);
  char* absent = path_join(root_dir, "missing", &arena);
  TEST_CHECK(fs_mkdir_p(present, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(plain_file, "x", 1, NULL, 0) == 0);

  TEST_CHECK(fs_require_dir(present, NULL, 0) == 0);

  char reason[FS_REASON_SIZE] = "";
  char message[FS_REASON_SIZE];
  char expected[FS_REASON_SIZE];
  TEST_CHECK(fs_require_dir(absent, reason, sizeof(reason)) == -1);
  const int absent_len = snprintf(expected, sizeof(expected), "%s ('%s')",
                                  expected_errno_reason(message, ENOENT), absent);
  TEST_CHECK(absent_len > 0 && (size_t)absent_len < sizeof(expected));
  TEST_CHECK(strcmp(reason, expected) == 0);

  // A wrong type is its own cause rather than an `errno`, so it carries no system message.
  reason[0] = '\0';
  TEST_CHECK(fs_require_dir(plain_file, reason, sizeof(reason)) == -1);
  const int type_len = snprintf(expected, sizeof(expected), "not a directory ('%s')", plain_file);
  TEST_CHECK(type_len > 0 && (size_t)type_len < sizeof(expected));
  TEST_CHECK(strcmp(reason, expected) == 0);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `fs_identify` answers which file a path names rather than what the path spells: two spellings of
// one file share an identity, two files do not. A path that cannot be inspected is rejected with
// the caller's struct left alone. The last of those is what `register_output_path` relies on to
// tell an output that does not exist yet from one that collides with a build input.
static void test_identify_distinguishes_files_and_rejects_missing(void) {
  char root_dir_template[] = "/tmp/fram-fs-identify.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* file_path = path_join(root_dir, "a.jpg", &arena);
  char* dotted_path = path_join(root_dir, "./a.jpg", &arena);
  char* other_path = path_join(root_dir, "b.jpg", &arena);
  TEST_CHECK(fs_write_file(file_path, "a", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(other_path, "b", 1, NULL, 0) == 0);

  struct FsIdentity identity;
  struct FsIdentity dotted_identity;
  TEST_CHECK(fs_identify(file_path, &identity) == 0);
  TEST_CHECK(fs_identify(dotted_path, &dotted_identity) == 0);
  TEST_CHECK(identity.device == dotted_identity.device);
  TEST_CHECK(identity.inode == dotted_identity.inode);

  // Two files in one directory share a device, so here it is the inode that has to tell them apart.
  // Neither number is predictable, so the assertion is relational rather than exact.
  struct FsIdentity other_identity;
  TEST_CHECK(fs_identify(other_path, &other_identity) == 0);
  TEST_CHECK(other_identity.device == identity.device);
  TEST_CHECK(other_identity.inode != identity.inode);

  // A path that names nothing has no identity, and the caller's struct is not written.
  struct FsIdentity unwritten = {.device = 7, .inode = 11};
  TEST_CHECK(fs_identify(path_join(root_dir, "missing.jpg", &arena), &unwritten) == -1);
  TEST_CHECK(unwritten.device == 7);
  TEST_CHECK(unwritten.inode == 11);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"list files matches suffix", test_list_files_matches_suffix},
    {"list files empty suffix matches all", test_list_files_empty_suffix_matches_all},
    {"list files matches suffix list and case", test_list_files_matches_suffix_list_and_case},
    {"list files accepts empty dir", test_list_files_accepts_empty_dir},
    {"list files skips symlink cycle", test_list_files_skips_symlink_cycle},
    {"list files skips symlink cycle to ancestor", test_list_files_skips_symlink_cycle_to_ancestor},
    {"list files walks aliased dir once", test_list_files_walks_aliased_dir_once},
    {"list files walks alias chain once", test_list_files_walks_alias_chain_once},
    {"list files skips excluded dir", test_list_files_skips_excluded_dir},
    {"list files walks tree deeper than open file limit",
     test_list_files_walks_tree_deeper_than_open_file_limit},
    {"list files rejects missing dir", test_list_files_rejects_missing_dir},
    {"list files rejects file root", test_list_files_rejects_file_root},
    {"list files rejects unstatable entry", test_list_files_rejects_unstatable_entry},
    {"read file accepts empty", test_read_file_accepts_empty},
    {"read file accepts file at limit", test_read_file_accepts_file_at_limit},
    {"read file accepts embedded nul", test_read_file_accepts_embedded_nul},
    {"read file rejects missing and non-regular", test_read_file_rejects_missing_and_non_regular},
    {"read file rejects fifo", test_read_file_rejects_fifo},
    {"read file rejects oversize before reading", test_read_file_rejects_oversize_before_reading},
    {"write then read round trips", test_write_then_read_round_trips},
    {"write file replaces contents", test_write_file_replaces_contents},
    {"writers apply umask and keep existing mode", test_writers_apply_umask_and_keep_existing_mode},
    {"write file retries interrupted write", test_write_file_retries_interrupted_write},
    {"write file rejects file parent and dir target",
     test_write_file_rejects_file_parent_and_dir_target},
    {"copy file streams binary bytes", test_copy_file_streams_binary_bytes},
    {"copy file rejects fifo source", test_copy_file_rejects_fifo_source},
    {"stat meta reports regular file", test_stat_meta_reports_regular_file},
    {"mkdir_p creates nested and is idempotent", test_mkdir_p_creates_nested_and_is_idempotent},
    {"mkdir_p rejects file component", test_mkdir_p_rejects_file_component},
    {"mkdir_p rejects unstatable component", test_mkdir_p_rejects_unstatable_component},
    {"mkdir_p rejects uncreatable component", test_mkdir_p_rejects_uncreatable_component},
    {"path exists separates absent from wrong type",
     test_path_exists_separates_absent_from_wrong_type},
    {"require dir reports why a directory is unusable",
     test_require_dir_reports_why_a_directory_is_unusable},
    {"identify distinguishes files and rejects missing",
     test_identify_distinguishes_files_and_rejects_missing},
    {NULL, NULL},
};
