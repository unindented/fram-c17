// `symlink` and `link` are POSIX rather than C17, so the POSIX build of this file asks for them.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/path.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

#if FRAM_TEST_OS_MEMORY
#include "os_memory.h"
#else
#include "runtime/fs.h"
#endif

// These are the contract tests of `runtime/os.h`. This one file builds two executables: one links
// the POSIX host in `fs.c` and runs against a real temporary directory, and one links the in-memory
// host in `tests/os_memory.c`. Both must pass. Every property that a test of another module checks
// against the in-memory host is checked here against both, so the in-memory host cannot accept what
// POSIX rejects. Facts only a real filesystem has, such as FIFOs, modes and interrupted writes, are
// in `test_fs.c`, which also produces, from permission bits, two failures the in-memory host can
// only inject: an entry that cannot be inspected and a directory that cannot be created. The
// in-memory build ends with tests of that injection, which the POSIX build has no counterpart to.
//
// Every call under test here runs with the operation `op`, so an expected failure reads
// `op: <cause>`. A system cause is spelled with `strerror` from the running libc, as both hosts
// spell it.

/** Read limit, in bytes, for the small fixture files most tests here read back. */
enum { TEST_FILE_LEN_MAX = 1024 };

/** Bytes of each standard stream `test_print_writes_each_stream_in_order` reads back. */
enum { TEST_PRINTED_SIZE = 64 };

/**
 * Symlink expansions one lookup follows before it fails with `ELOOP`, as the kernel the tests run
 * on does: 32 on macOS and 40 on Linux.
 */
#ifdef __APPLE__
enum { TEST_LINK_EXPANSION_COUNT_MAX = 32 };
#else
enum { TEST_LINK_EXPANSION_COUNT_MAX = 40 };
#endif

/** Bytes a streamed write test sends, past three buffers of the POSIX host's 64 KiB. */
enum { TEST_STREAM_LEN = 200 * 1024 };

/** One test's host and the directory its tree lives in. */
struct Contract {
  /** Host under test. */
  struct Os* os;

  /** Terminated path of the test's directory: a new temporary directory, or a fixed one. */
  const char* root_dir;

  /** Arena for paths and read data. */
  struct Arena arena;
};

/**
 * @brief Starts a test: sets up the host and an empty directory for its tree.
 *
 * @param root_template Writable `mkdtemp` template. The POSIX host creates the directory there. The
 *                      in-memory host uses the template as it stands. Must not be `NULL`.
 * @param contract_out  Receives the contract. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure when the directory could
 *         not be created.
 */
static int contract_begin(char root_template[static 1], struct Contract* contract_out) {
  *contract_out = (struct Contract){.root_dir = root_template, .arena = test_arena()};
#if FRAM_TEST_OS_MEMORY
  contract_out->os = os_memory();
  os_memory_mkdir(contract_out->os, root_template);
  return 0;
#else
  contract_out->os = os_posix();
  return init_fixture_dir(root_template);
#endif
}

/**
 * @brief Ends a test: removes the POSIX host's directory.
 *
 * @param contract Contract that `contract_begin` set up. Must not be `NULL`.
 */
static void contract_end(const struct Contract* contract) {
#if FRAM_TEST_OS_MEMORY
  (void)contract;
#else
  remove_fixture_tree(contract->root_dir);
#endif
}

/**
 * @brief Returns `relative_path` below the test's directory, with a terminator after it.
 *
 * @param contract      Contract of the test. Must not be `NULL`.
 * @param relative_path Terminated path below the directory. Must not be `NULL`.
 * @return The joined path, its `data` terminated.
 */
static struct Str contract_path(struct Contract* contract, const char* relative_path) {
  const struct Str path =
      path_join(str_from_cstr(contract->root_dir), str_from_cstr(relative_path), &contract->arena);
  return (struct Str){str_cstr(path, &contract->arena), path.len};
}

/**
 * @brief Writes a fixture file below the test's directory, creating its parents.
 *
 * @param contract      Contract of the test. Must not be `NULL`.
 * @param relative_path Terminated path below the directory. Must not be `NULL`.
 * @param data          Bytes to write.
 */
static void contract_write(struct Contract* contract, const char* relative_path, struct Str data) {
  struct Log log = test_log();
  TEST_CHECK(os_write(contract->os, contract_path(contract, relative_path),
                      (struct OsSource){.bytes = data}, S("failed to write fixture"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

/**
 * @brief Extends a file below the test's directory to `len` bytes, as `truncate` does.
 *
 * @param contract      Contract of the test. Must not be `NULL`.
 * @param relative_path Terminated path of an existing file below the directory. Must not be `NULL`.
 * @param len           New size in bytes, at least the file's size.
 */
static void contract_truncate(struct Contract* contract, const char* relative_path, ptrdiff_t len) {
  const struct Str file_path = contract_path(contract, relative_path);
#if FRAM_TEST_OS_MEMORY
  os_memory_truncate(contract->os, file_path.data, len);
#else
  TEST_CHECK(truncate(file_path.data, (off_t)len) == 0);
#endif
}

/**
 * @brief Creates a symlink below the test's directory.
 *
 * @param contract           Contract of the test. Must not be `NULL`.
 * @param target             Terminated target, stored as it stands. Must not be `NULL`.
 * @param relative_link_path Terminated path of the link below the directory. Must not be `NULL`.
 */
static void contract_symlink(struct Contract* contract,
                             const char* target,
                             const char* relative_link_path) {
  const struct Str link_path = contract_path(contract, relative_link_path);
#if FRAM_TEST_OS_MEMORY
  os_memory_symlink(contract->os, target, link_path.data);
#else
  TEST_CHECK(symlink(target, link_path.data) == 0);
#endif
}

/**
 * @brief Creates a hard link below the test's directory to a file there.
 *
 * @param contract           Contract of the test. Must not be `NULL`.
 * @param relative_file_path Terminated path of the existing file below the directory. Must not be
 *                           `NULL`.
 * @param relative_link_path Terminated path of the new link below the directory. Must not be
 *                           `NULL`.
 */
static void contract_link(struct Contract* contract,
                          const char* relative_file_path,
                          const char* relative_link_path) {
  const struct Str file_path = contract_path(contract, relative_file_path);
  const struct Str link_path = contract_path(contract, relative_link_path);
#if FRAM_TEST_OS_MEMORY
  os_memory_link(contract->os, file_path.data, link_path.data);
#else
  TEST_CHECK(link(file_path.data, link_path.data) == 0);
#endif
}

/**
 * @brief Reads a file below the test's directory, and reports whether it holds `expected`.
 *
 * @param contract      Contract of the test. Must not be `NULL`.
 * @param relative_path Terminated path below the directory. Must not be `NULL`.
 * @param expected      Bytes the file must hold.
 * @return `true` when the read succeeds with exactly `expected`.
 */
static bool contract_is_file(struct Contract* contract,
                             const char* relative_path,
                             struct Str expected) {
  struct Log log = test_log();
  struct Str data = {0};
  const bool is_read = os_read(contract->os, contract_path(contract, relative_path),
                               TEST_FILE_LEN_MAX, &contract->arena, &data, S("op"), &log) == 0;
  const struct Str failure = buf_str(&log.buf);
  TEST_MSG("%.*s", STR_FMT(failure));
  return is_read && str_eq(data, expected);
}

/**
 * @brief Returns the identity of a path below the test's directory, or a zero identity when it
 *        cannot be inspected.
 *
 * @param contract      Contract of the test. Must not be `NULL`.
 * @param relative_path Terminated path below the directory. Must not be `NULL`.
 * @return The identity.
 */
static struct OsIdentity contract_identity(struct Contract* contract, const char* relative_path) {
  struct OsStat st = {0};
  TEST_CHECK_(os_stat(contract->os, contract_path(contract, relative_path), &st) == 0,
              "'%s' can be inspected", relative_path);
  return st.identity;
}

/**
 * @brief Reports whether two identities name the same file.
 *
 * @param a First identity.
 * @param b Second identity.
 * @return `true` when both the device and the inode agree.
 */
static bool is_identity_equal(struct OsIdentity a, struct OsIdentity b) {
  return a.device == b.device && a.inode == b.inode;
}

/**
 * @brief Counts the entries the test's directory holds, recording why when it cannot be listed.
 *
 * @param contract  Contract of the test. Must not be `NULL`.
 * @param count_out Receives the entry count. Written only on success. Must not be `NULL`.
 * @return `0` on success, or `-1` when the directory cannot be listed.
 */
static int contract_entry_count(struct Contract* contract, ptrdiff_t* count_out) {
  struct OsEntry* entries = NULL;
  struct Log log = test_log();
  const int rc = os_list(contract->os, str_from_cstr(contract->root_dir), &contract->arena,
                         &entries, count_out, S("op"), &log);
  const struct Str failure = buf_str(&log.buf);
  TEST_MSG("%.*s", STR_FMT(failure));
  return rc;
}

/**
 * @brief Returns the entry of a listing named `name`, or `NULL`.
 *
 * @param entries Entries of the listing. Must not be `NULL` when `count` is positive.
 * @param count   Number of entries.
 * @param name    Terminated name to look for. Must not be `NULL`.
 * @return The entry, or `NULL` when the listing has none of that name.
 */
static const struct OsEntry* entry_named(const struct OsEntry* entries,
                                         ptrdiff_t count,
                                         const char* name) {
  for (ptrdiff_t i = 0; i < count; i++) {
    if (str_eq(entries[i].name, str_from_cstr(name))) {
      return &entries[i];
    }
  }
  return NULL;
}

// An empty file reads back as zero bytes, and a file exactly at `data_len_max` bytes reads in full,
// so the limit is inclusive.
static void test_read_accepts_empty_and_file_at_limit(void) {
  char root[] = "/tmp/fram-os-read-limit.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  struct Log log = test_log();
  contract_write(&c, "empty.md", S(""));
  contract_write(&c, "four.md", S("four"));

  struct Str data = S("unchanged");
  TEST_CHECK(os_read(c.os, contract_path(&c, "empty.md"), TEST_FILE_LEN_MAX, &c.arena, &data,
                     S("op"), &log) == 0);
  TEST_CHECK(data.len == 0);
  TEST_CHECK(os_read(c.os, contract_path(&c, "four.md"), S("four").len, &c.arena, &data, S("op"),
                     &log) == 0);
  TEST_CHECK(str_eq(data, S("four")));
  TEST_CHECK(test_log_is_empty(&log));
  contract_end(&c);
}

// A file holding `NUL` bytes, as every JPEG does, reads back whole, and `perm` ends right past it.
static void test_read_accepts_embedded_nul(void) {
  char root[] = "/tmp/fram-os-read-nul.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "nul.md", S("before\0after"));
  const struct Str path = contract_path(&c, "nul.md");

  struct Log log = test_log();
  struct Str data = {0};
  TEST_CHECK(os_read(c.os, path, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == 0);
  TEST_CHECK(c.arena.beg == data.data + data.len);
  TEST_CHECK(str_eq(data, S("before\0after")));
  TEST_CHECK(test_log_is_empty(&log));
  contract_end(&c);
}

// A path one byte below `PATH_MAX` writes and reads its file, so the limit rejects only a path the
// system could not take whole, even where the file's name is short. The path is `./` and `/.` pairs
// in front of a file in the working directory, which is the test's directory on the POSIX host and
// the root on the in-memory one.
static void test_read_and_write_accept_path_below_path_max(void) {
  static char long_path[PATH_MAX - 1];
  const struct Str name = S("/a.md");
  long_path[0] = '.';
  ptrdiff_t len = 1;
  // A doubled `/` evens out the padding, so the `/.` pairs end exactly where the name starts.
  if ((PATH_MAX - 1 - len - name.len) % 2 != 0) {
    long_path[len++] = '/';
  }
  while (len < PATH_MAX - 1 - name.len) {
    long_path[len++] = '/';
    long_path[len++] = '.';
  }
  memcpy(long_path + len, name.data, (size_t)name.len);

  char root[] = "/tmp/fram-os-path-limit.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  struct Log log = test_log();
  struct Str data = {0};
  const struct Str long_path_str = {long_path, PATH_MAX - 1};
  int rc = -1;
#if !FRAM_TEST_OS_MEMORY
  int saved_dir_fd = -1;
  if (working_dir_enter(c.root_dir, &saved_dir_fd) != 0) {
    goto cleanup;
  }
#endif
  TEST_CHECK(os_write(c.os, long_path_str, (struct OsSource){.bytes = S("a")}, S("op"), &log) == 0);
  rc = os_read(c.os, long_path_str, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log);
#if !FRAM_TEST_OS_MEMORY
  (void)working_dir_leave(saved_dir_fd);
#endif
  TEST_CHECK(rc == 0);
  TEST_CHECK(str_eq(data, S("a")));
  TEST_CHECK(test_log_is_empty(&log));

#if !FRAM_TEST_OS_MEMORY
cleanup:
#endif
  contract_end(&c);
}

// A read follows a symlink to its file, and a symlink cycle fails with the system's message.
static void test_read_follows_symlinks_and_rejects_cycle(void) {
  char root[] = "/tmp/fram-os-read-link.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "dir/a.md", S("a"));
  contract_symlink(&c, "dir/a.md", "file-link.md");
  contract_symlink(&c, "dir", "dir-link");
  contract_symlink(&c, "loop-b", "loop-a");
  contract_symlink(&c, "loop-a", "loop-b");
  TEST_CHECK(contract_is_file(&c, "file-link.md", S("a")));
  TEST_CHECK(contract_is_file(&c, "dir-link/a.md", S("a")));
  TEST_CHECK(contract_is_file(&c, "dir-link/../dir/a.md", S("a")));

  struct Str data = {0};
  struct Log log = test_log();
  const struct Str loop = contract_path(&c, "loop-a");
  TEST_CHECK(os_read(c.os, loop, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(ELOOP), STR_FMT(loop)));
  contract_end(&c);
}

// A lookup follows a chain of as many symlinks as the kernel does and fails on a chain one link
// longer with the system's message, so the in-memory host accepts no chain the POSIX host rejects.
// The chain is read relative to the test's directory, the working directory on the POSIX host,
// because a symlink in the path to that directory, as `/tmp` is on macOS, counts toward the limit.
static void test_read_follows_symlink_chain_up_to_system_limit(void) {
  char root[] = "/tmp/fram-os-read-chain.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  enum { LINK_NAME_SIZE = 16 };
  contract_write(&c, "l0", S("a"));
  for (int i = 1; i <= TEST_LINK_EXPANSION_COUNT_MAX + 1; i++) {
    char target[LINK_NAME_SIZE];
    char link_name[LINK_NAME_SIZE];
    (void)snprintf(target, sizeof(target), "l%d", i - 1);
    (void)snprintf(link_name, sizeof(link_name), "l%d", i);
    contract_symlink(&c, target, link_name);
  }
  char at_limit[LINK_NAME_SIZE];
  char past_limit[LINK_NAME_SIZE];
  (void)snprintf(at_limit, sizeof(at_limit), "l%d", TEST_LINK_EXPANSION_COUNT_MAX);
  (void)snprintf(past_limit, sizeof(past_limit), "l%d", TEST_LINK_EXPANSION_COUNT_MAX + 1);

#if FRAM_TEST_OS_MEMORY
  const struct Str dir = str_from_cstr(c.root_dir);
#else
  const struct Str dir = S(".");
#endif
  const struct Str at_limit_path = path_join(dir, str_from_cstr(at_limit), &c.arena);
  const struct Str past_limit_path = path_join(dir, str_from_cstr(past_limit), &c.arena);
  struct Log log = test_log();
  struct Log past_limit_log = test_log();
  struct Str data = {0};
  int at_limit_rc = -1;
  int past_limit_rc = 0;
#if !FRAM_TEST_OS_MEMORY
  int saved_dir_fd = -1;
  if (working_dir_enter(c.root_dir, &saved_dir_fd) != 0) {
    goto cleanup;
  }
#endif
  at_limit_rc = os_read(c.os, at_limit_path, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log);
  past_limit_rc =
      os_read(c.os, past_limit_path, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &past_limit_log);
#if !FRAM_TEST_OS_MEMORY
  (void)working_dir_leave(saved_dir_fd);
#endif
  TEST_CHECK(at_limit_rc == 0);
  TEST_CHECK(str_eq(data, S("a")));
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(past_limit_rc == -1);
  TEST_CHECK(
      test_log_is(&past_limit_log, "op: %s ('%.*s')\n", strerror(ELOOP), STR_FMT(past_limit_path)));

#if !FRAM_TEST_OS_MEMORY
cleanup:
#endif
  contract_end(&c);
}

// A file one byte over `data_len_max` is rejected with the limit and the size, and the output is
// left as it was.
static void test_read_rejects_oversize(void) {
  char root[] = "/tmp/fram-os-read-oversize.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  struct Log log = test_log();
  contract_write(&c, "five.md", S("five!"));

  const struct Str sentinel = S("unchanged");
  struct Str data = sentinel;
  const struct Str path = contract_path(&c, "five.md");
  TEST_CHECK(os_read(c.os, path, 4, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(data.data == sentinel.data && data.len == sentinel.len);
  TEST_CHECK(test_log_is(&log, "op: exceeds max file size (4 bytes) at 5 bytes ('%.*s')\n",
                         STR_FMT(path)));
  contract_end(&c);
}

// A file extended past its bytes has its whole size checked against the limit before any byte is
// read, and a stat reports that size. The file is sparse on disk, so the size costs nothing. Within
// the limit, the bytes past the old end read back as `NUL` bytes.
static void test_read_checks_size_of_extended_file(void) {
  enum { BIG_LEN = 1024 * 1024 * 1024 };
  char root[] = "/tmp/fram-os-read-extended.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "big.md", S("a"));
  contract_truncate(&c, "big.md", BIG_LEN);
  contract_write(&c, "small.md", S("a"));
  contract_truncate(&c, "small.md", 4);

  const struct Str big = contract_path(&c, "big.md");
  struct Str data = {0};
  struct Log log = test_log();
  TEST_CHECK(os_read(c.os, big, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: exceeds max file size (%d bytes) at %d bytes ('%.*s')\n",
                         TEST_FILE_LEN_MAX, BIG_LEN, STR_FMT(big)));

  struct OsStat st;
  TEST_CHECK(os_stat(c.os, big, &st) == 0 && st.len == BIG_LEN);
  log = test_log();

  const struct Str small = contract_path(&c, "small.md");
  TEST_CHECK(os_read(c.os, small, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == 0);
  TEST_CHECK(str_eq(data, S("a\0\0\0")));
  TEST_CHECK(test_log_is_empty(&log));
  contract_end(&c);
}

// A missing path fails with the system's message, and a directory is not a regular file, which is a
// first-party message.
static void test_read_rejects_missing_and_directory(void) {
  char root[] = "/tmp/fram-os-read-missing.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  const struct Str missing = contract_path(&c, "missing.md");
  const struct Str sentinel = S("unchanged");
  struct Str data = sentinel;
  struct Log log = test_log();
  TEST_CHECK(os_read(c.os, missing, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(data.data == sentinel.data && data.len == sentinel.len);
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(ENOENT), STR_FMT(missing)));

  log = test_log();
  TEST_CHECK(os_read(c.os, str_from_cstr(c.root_dir), TEST_FILE_LEN_MAX, &c.arena, &data, S("op"),
                     &log) == -1);
  TEST_CHECK(data.data == sentinel.data && data.len == sentinel.len);
  TEST_CHECK(test_log_is(&log, "op: not a regular file ('%s')\n", c.root_dir));
  contract_end(&c);
}

// A path holding a `NUL` byte fails as `EINVAL`, and a path of `PATH_MAX` bytes fails as
// `ENAMETOOLONG`, even when the bytes before the `NUL` name a real file.
static void test_read_rejects_path_with_nul_and_path_at_path_max(void) {
  char root[] = "/tmp/fram-os-read-path.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "a.md", S("a"));
  const struct Str nul_path = str_concat(contract_path(&c, "a.md"), S("\0b"), &c.arena);

  struct Str data = {0};
  struct Log log = test_log();
  TEST_CHECK(os_read(c.os, nul_path, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  // `%.*s` stops at the first `NUL`, so the message quotes the path only up to it.
  const struct Str nul_path_head = contract_path(&c, "a.md");
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(EINVAL), STR_FMT(nul_path_head)));

  static char long_path[PATH_MAX];
  long_path[0] = '/';
  for (ptrdiff_t i = 1; i < (ptrdiff_t)sizeof(long_path); i++) {
    long_path[i] = i % 64 == 0 ? '/' : 'a';
  }
  const struct Str long_path_str = {long_path, PATH_MAX};
  log = test_log();
  TEST_CHECK(os_read(c.os, long_path_str, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "op: %s ('%.*s')\n", strerror(ENAMETOOLONG), STR_FMT(long_path_str)));
  contract_end(&c);
}

// A path that ends in `/` must name a directory. A file, or a symlink to one, fails as `ENOTDIR` in
// a read, a listing and a stat, and so does a file followed by `/.`. A directory, or a symlink to
// one, followed by `/` is the directory.
static void test_trailing_slash_requires_directory(void) {
  char root[] = "/tmp/fram-os-trailing-slash.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "file", S("x"));
  contract_write(&c, "dir/a.md", S("a"));
  contract_symlink(&c, "file", "file-link");
  contract_symlink(&c, "dir", "dir-link");

  static const char* const not_dirs[] = {"file/", "file-link/", "file/."};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(not_dirs) / sizeof(not_dirs[0])); i++) {
    TEST_CASE(not_dirs[i]);
    const struct Str path = contract_path(&c, not_dirs[i]);
    struct Str data = {0};
    struct Log log = test_log();
    TEST_CHECK(os_read(c.os, path, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
    TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(ENOTDIR), STR_FMT(path)));

    struct OsEntry* entries = NULL;
    ptrdiff_t count = 0;
    log = test_log();
    TEST_CHECK(os_list(c.os, path, &c.arena, &entries, &count, S("op"), &log) == -1);
    TEST_CHECK(test_log_is(&log, "op: cannot open directory: %s ('%.*s')\n", strerror(ENOTDIR),
                           STR_FMT(path)));

    struct OsStat st;
    errno = 0;
    TEST_CHECK(os_stat(c.os, path, &st) == -1);
    TEST_CHECK(errno == ENOTDIR);
  }
  TEST_CASE_(NULL);

  // A write to `file/` needs `file` as a directory, so it fails where `os_mkdirs` would.
  const struct Str file = contract_path(&c, "file");
  struct Log log = test_log();
  TEST_CHECK(os_write(c.os, contract_path(&c, "file/"), (struct OsSource){.bytes = S("y")}, S("op"),
                      &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: exists and is not a directory ('%.*s')\n", STR_FMT(file)));
  TEST_CHECK(contract_is_file(&c, "file", S("x")));

  struct OsStat st;
  TEST_CHECK(os_stat(c.os, contract_path(&c, "dir/"), &st) == 0 && st.is_dir);
  TEST_CHECK(os_stat(c.os, contract_path(&c, "dir-link/"), &st) == 0 && st.is_dir);
  contract_end(&c);
}

// A write creates the missing parent directories and reads back byte for byte. The bytes are the
// first allocation in `perm`'s free space, which then begins right past them.
static void test_write_creates_parents_and_round_trips(void) {
  char root[] = "/tmp/fram-os-write.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  const struct Str path = contract_path(&c, "a/b/c.html");
  struct Log log = test_log();
  TEST_CHECK(os_write(c.os, path, (struct OsSource){.bytes = S("deep")}, S("op"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));

  struct OsStat st;
  TEST_CHECK(os_stat(c.os, contract_path(&c, "a/b"), &st) == 0 && st.is_dir);

  struct Str data = {0};
  struct Arena probe = c.arena;
  char* const perm_beg = arena_new(&probe, 0, char);
  TEST_CHECK(os_read(c.os, path, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == 0);
  TEST_CHECK(data.data == perm_beg);
  TEST_CHECK(c.arena.beg == data.data + S("deep").len);
  TEST_CHECK(str_eq(data, S("deep")));
  TEST_CHECK(test_log_is_empty(&log));
  contract_end(&c);
}

// A second write replaces the contents, including bytes past the new end. It goes through the file
// already there, so a hard link to it and a symlink to it see the new contents.
static void test_write_overwrites_in_place(void) {
  char root[] = "/tmp/fram-os-overwrite.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "out.html", S("old contents"));
  contract_link(&c, "out.html", "twin.html");
  contract_symlink(&c, "out.html", "link.html");
  contract_write(&c, "out.html", S("new"));
  TEST_CHECK(contract_is_file(&c, "out.html", S("new")));
  TEST_CHECK(contract_is_file(&c, "twin.html", S("new")));
  contract_write(&c, "link.html", S("newer"));
  TEST_CHECK(contract_is_file(&c, "out.html", S("newer")));
  TEST_CHECK(contract_is_file(&c, "twin.html", S("newer")));
  contract_end(&c);
}

// A file source is copied byte for byte, `NUL` bytes and all, across several of the POSIX host's
// buffers.
static void test_write_copies_file_source(void) {
  char root[] = "/tmp/fram-os-write-sources.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  char* bytes = arena_new(&c.arena, TEST_STREAM_LEN, char);
  for (ptrdiff_t i = 0; i < TEST_STREAM_LEN; i++) {
    bytes[i] = (char)(i * 7 % 251);
  }
  const struct Str data = {bytes, TEST_STREAM_LEN};
  struct Log log = test_log();
  contract_write(&c, "source.jpg", data);
  const struct OsSource file = {.kind = OS_SOURCE_FILE,
                                .file_path = contract_path(&c, "source.jpg")};
  TEST_CHECK(os_write(c.os, contract_path(&c, "copy/source.jpg"), file, S("op"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  struct Str read = {0};
  TEST_CHECK(os_read(c.os, contract_path(&c, "copy/source.jpg"), TEST_STREAM_LEN, &c.arena, &read,
                     S("op"), &log) == 0);
  TEST_CHECK(str_eq(read, data));
  contract_end(&c);
}

// A write through a symlink writes the file it names, from every source, and leaves the symlink a
// symlink.
static void test_write_follows_symlink_for_every_source(void) {
  char root[] = "/tmp/fram-os-write-symlink.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "out.jpg", S("old"));
  contract_write(&c, "source.jpg", S("copied"));
  contract_symlink(&c, "out.jpg", "link.jpg");
  const struct Str link = contract_path(&c, "link.jpg");
  struct Log log = test_log();
  TEST_CHECK(os_write(c.os, link,
                      (struct OsSource){.kind = OS_SOURCE_FILE,
                                        .file_path = contract_path(&c, "source.jpg")},
                      S("op"), &log) == 0);
  TEST_CHECK(contract_is_file(&c, "out.jpg", S("copied")));
  TEST_CHECK(os_write(c.os, link, (struct OsSource){.bytes = S("bytes")}, S("op"), &log) == 0);
  TEST_CHECK(contract_is_file(&c, "out.jpg", S("bytes")));
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(
      is_identity_equal(contract_identity(&c, "link.jpg"), contract_identity(&c, "out.jpg")));
  ptrdiff_t entry_count = 0;
  TEST_CHECK(contract_entry_count(&c, &entry_count) == 0 && entry_count == 3);
  contract_end(&c);
}

// A write to a name at the system's 255-byte limit succeeds, and leaves nothing beside the output.
static void test_write_accepts_name_at_name_max(void) {
  char root[] = "/tmp/fram-os-write-name-max.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  char name[FILENAME_LEN_MAX + 1];
  memset(name, 'a', FILENAME_LEN_MAX - 4);
  memcpy(name + FILENAME_LEN_MAX - 4, ".jpg", sizeof(".jpg"));
  contract_write(&c, name, S("old"));
  struct Log log = test_log();
  TEST_CHECK(os_write(c.os, contract_path(&c, name), (struct OsSource){.bytes = S("new")}, S("op"),
                      &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(contract_is_file(&c, name, S("new")));
  ptrdiff_t entry_count = 0;
  TEST_CHECK(contract_entry_count(&c, &entry_count) == 0 && entry_count == 1);
  contract_end(&c);
}

// A write fails when a parent is a file, naming that parent, and when the target is a directory,
// with the system's message.
static void test_write_rejects_file_parent_and_dir_target(void) {
  char root[] = "/tmp/fram-os-write-fail.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "plain.txt", S("x"));
  const struct Str plain = contract_path(&c, "plain.txt");
  struct Log log = test_log();
  TEST_CHECK(os_write(c.os, contract_path(&c, "plain.txt/sub/child.html"),
                      (struct OsSource){.bytes = S("x")}, S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: exists and is not a directory ('%.*s')\n", STR_FMT(plain)));

  const struct Str dir = contract_path(&c, "sub");
  TEST_CHECK(os_mkdirs(c.os, dir, S("op"), &log) == 0);
  log = test_log();
  TEST_CHECK(os_write(c.os, dir, (struct OsSource){.bytes = S("x")}, S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(EISDIR), STR_FMT(dir)));
  contract_end(&c);
}

// A write and a directory creation on a path holding a `NUL` byte fail as `EINVAL`, quoting the
// path up to that byte, and create nothing at the bytes before it.
static void test_write_and_mkdirs_reject_path_with_nul(void) {
  char root[] = "/tmp/fram-os-write-nul.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  const struct Str file_path = contract_path(&c, "a.md");
  struct Log log = test_log();
  TEST_CHECK(os_write(c.os, str_concat(file_path, S("\0b"), &c.arena),
                      (struct OsSource){.bytes = S("a")}, S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(EINVAL), STR_FMT(file_path)));

  const struct Str dir_path = contract_path(&c, "sub");
  log = test_log();
  TEST_CHECK(os_mkdirs(c.os, str_concat(dir_path, S("\0b"), &c.arena), S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: cannot create directory: %s ('%.*s')\n", strerror(EINVAL),
                         STR_FMT(dir_path)));

  struct OsStat st;
  TEST_CHECK(os_stat(c.os, file_path, &st) == -1);
  TEST_CHECK(os_stat(c.os, dir_path, &st) == -1);
  contract_end(&c);
}

// A file source that is missing or not a regular file fails naming the source, and creates neither
// the destination nor its parent.
static void test_write_rejects_missing_and_directory_source(void) {
  char root[] = "/tmp/fram-os-write-source-fail.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  const struct Str missing = contract_path(&c, "missing.jpg");
  struct Log log = test_log();
  TEST_CHECK(os_write(c.os, contract_path(&c, "out/a.jpg"),
                      (struct OsSource){.kind = OS_SOURCE_FILE, .file_path = missing}, S("op"),
                      &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(ENOENT), STR_FMT(missing)));

  log = test_log();
  TEST_CHECK(
      os_write(c.os, contract_path(&c, "out/b.jpg"),
               (struct OsSource){.kind = OS_SOURCE_FILE, .file_path = str_from_cstr(c.root_dir)},
               S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: not a regular file ('%s')\n", c.root_dir));
  struct OsStat st;
  TEST_CHECK(os_stat(c.os, contract_path(&c, "out"), &st) == -1);
  contract_end(&c);
}

// Every source fails on a directory target with the system's message, and leaves the directory and
// the test's directory as they were.
static void test_write_rejects_dir_target_for_every_source(void) {
  char root[] = "/tmp/fram-os-write-dir-target.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "source.jpg", S("x"));
  const struct Str dir = contract_path(&c, "dir");
  struct Log log = test_log();
  TEST_CHECK(os_mkdirs(c.os, dir, S("op"), &log) == 0);
  const struct OsSource sources[] = {
      {.bytes = S("x")},
      {.kind = OS_SOURCE_FILE, .file_path = contract_path(&c, "source.jpg")},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(sources) / sizeof(sources[0])); i++) {
    log = test_log();
    TEST_CHECK(os_write(c.os, dir, sources[i], S("op"), &log) == -1);
    TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(EISDIR), STR_FMT(dir)));
  }
  struct OsStat st;
  TEST_CHECK(os_stat(c.os, dir, &st) == 0 && st.is_dir);
  ptrdiff_t entry_count = 0;
  TEST_CHECK(contract_entry_count(&c, &entry_count) == 0 && entry_count == 2);
  contract_end(&c);
}

// `os_mkdirs` creates a deep path, treats an empty path as a no-op, is idempotent, and accepts a
// symlink to a directory as a component.
static void test_mkdirs_creates_nested_and_is_idempotent(void) {
  char root[] = "/tmp/fram-os-mkdirs.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  struct Log log = test_log();
  TEST_CHECK(os_mkdirs(c.os, S(""), S("op"), &log) == 0);
  const struct Str nested = contract_path(&c, "x/y/z");
  TEST_CHECK(os_mkdirs(c.os, nested, S("op"), &log) == 0);
  TEST_CHECK(os_mkdirs(c.os, nested, S("op"), &log) == 0);
  struct OsStat st;
  TEST_CHECK(os_stat(c.os, nested, &st) == 0 && st.is_dir);

  contract_symlink(&c, "x", "x-link");
  TEST_CHECK(os_mkdirs(c.os, contract_path(&c, "x-link/y/w"), S("op"), &log) == 0);
  TEST_CHECK(os_stat(c.os, contract_path(&c, "x/y/w"), &st) == 0 && st.is_dir);
  TEST_CHECK(test_log_is_empty(&log));
  contract_end(&c);
}

// `os_mkdirs` fails on a component that is a file, naming that component whether it is the whole
// path or a parent, and on a dangling symlink, which exists but cannot be inspected.
static void test_mkdirs_rejects_file_and_dangling_components(void) {
  char root[] = "/tmp/fram-os-mkdirs-fail.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "file", S("x"));
  const struct Str file = contract_path(&c, "file");
  struct Log log = test_log();
  TEST_CHECK(os_mkdirs(c.os, file, S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: exists and is not a directory ('%.*s')\n", STR_FMT(file)));
  log = test_log();
  TEST_CHECK(os_mkdirs(c.os, contract_path(&c, "file/child"), S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: exists and is not a directory ('%.*s')\n", STR_FMT(file)));

  contract_symlink(&c, "/fram-nonexistent-symlink-target", "dangling");
  const struct Str dangling = contract_path(&c, "dangling");
  log = test_log();
  TEST_CHECK(os_mkdirs(c.os, dangling, S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: cannot inspect directory: %s ('%.*s')\n", strerror(ENOENT),
                         STR_FMT(dangling)));
  contract_end(&c);
}

// A listing classifies each entry after following a symlink, and leaves out `.`, `..`, a dangling
// symlink and a symlink cycle. Its order is not specified, so each entry is looked up by name.
static void test_list_classifies_entries_and_skips_links_to_nothing(void) {
  char root[] = "/tmp/fram-os-list.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "dir/a.md", S("a"));
  contract_write(&c, "file.md", S("three"));
  contract_symlink(&c, "dir", "dir-link");
  contract_symlink(&c, "file.md", "file-link.md");
  contract_symlink(&c, "missing", "dangling");
  contract_symlink(&c, "self", "self");
  contract_symlink(&c, "loop-b", "loop-a");
  contract_symlink(&c, "loop-a", "loop-b");

  struct OsEntry* entries = NULL;
  ptrdiff_t count = 0;
  struct Log log = test_log();
  TEST_CHECK(os_list(c.os, str_from_cstr(c.root_dir), &c.arena, &entries, &count, S("op"), &log) ==
             0);
  TEST_CHECK(test_log_is_empty(&log));
  static const struct {
    const char* name;
    enum OsEntryKind kind;
  } expected[] = {{"dir", OS_ENTRY_DIR},
                  {"dir-link", OS_ENTRY_DIR_LINK},
                  {"file.md", OS_ENTRY_FILE},
                  {"file-link.md", OS_ENTRY_FILE}};
  TEST_CHECK(count == (ptrdiff_t)(sizeof(expected) / sizeof(expected[0])));
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(expected) / sizeof(expected[0])); i++) {
    const struct OsEntry* entry = entry_named(entries, count, expected[i].name);
    TEST_CHECK_(entry != NULL && entry->kind == expected[i].kind && entry->error_number == 0,
                "'%s' is listed with its kind", expected[i].name);
  }
  contract_end(&c);
}

// A listing of a missing path or of a file fails at the open, with the system's message.
static void test_list_rejects_missing_and_file(void) {
  char root[] = "/tmp/fram-os-list-fail.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "file", S("x"));
  static const struct {
    const char* path;
    int error_number;
  } cases[] = {{"missing", ENOENT}, {"file", ENOTDIR}};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i].path);
    const struct Str path = contract_path(&c, cases[i].path);
    struct OsEntry* entries = NULL;
    ptrdiff_t count = 0;
    struct Log log = test_log();
    TEST_CHECK(os_list(c.os, path, &c.arena, &entries, &count, S("op"), &log) == -1);
    TEST_CHECK(test_log_is(&log, "op: cannot open directory: %s ('%.*s')\n",
                           strerror(cases[i].error_number), STR_FMT(path)));
  }
  contract_end(&c);
}

// A symlink through a regular file names no directory entry the listing could keep or skip, so the
// listing returns it as an entry that could not be inspected, with its `errno`, and reports nothing
// itself. The caller reports it where its own order reaches it.
static void test_list_marks_link_through_file_failed(void) {
  char root[] = "/tmp/fram-os-list-link.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "file.md", S("x"));
  contract_symlink(&c, "file.md/y", "link");
  struct OsEntry* entries = NULL;
  ptrdiff_t count = 0;
  struct Log log = test_log();
  TEST_CHECK(os_list(c.os, str_from_cstr(c.root_dir), &c.arena, &entries, &count, S("op"), &log) ==
             0);
  TEST_CHECK(test_log_is_empty(&log));
  const struct OsEntry* entry = entry_named(entries, count, "link");
  TEST_CHECK(entry != NULL && entry->kind == OS_ENTRY_FAILED && entry->error_number == ENOTDIR);
  TEST_CHECK(entry_named(entries, count, "file.md") != NULL);
  contract_end(&c);
}

// `os_stat` reports a file's size, and a file written after another is at least as new, whether the
// write created it or overwrote it, which is what a freshness check relies on.
static void test_stat_reports_size_and_write_order(void) {
  char root[] = "/tmp/fram-os-stat-time.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "overwritten.jpg", S("old"));
  contract_write(&c, "source.jpg", S("source"));
  contract_write(&c, "created.jpg", S("output"));
  contract_write(&c, "overwritten.jpg", S("out"));
  struct OsStat source;
  struct OsStat created;
  struct OsStat overwritten;
  TEST_CHECK(os_stat(c.os, contract_path(&c, "source.jpg"), &source) == 0);
  TEST_CHECK(os_stat(c.os, contract_path(&c, "created.jpg"), &created) == 0);
  TEST_CHECK(os_stat(c.os, contract_path(&c, "overwritten.jpg"), &overwritten) == 0);
  TEST_CHECK(source.len == 6 && created.len == 6 && overwritten.len == 3);
  TEST_CHECK(!source.is_dir && source.mtime_ns >= 0 && source.mtime_ns < 1000000000);
  const struct OsStat* const later[] = {&created, &overwritten};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(later) / sizeof(later[0])); i++) {
    TEST_CHECK(later[i]->mtime_s > source.mtime_s ||
               (later[i]->mtime_s == source.mtime_s && later[i]->mtime_ns >= source.mtime_ns));
  }
  contract_end(&c);
}

// `os_stat` answers which file a path names: a `./` spelling, a `..` spelling, a symlink, a symlink
// in a subdirectory whose `../` target resolves from that subdirectory, and a hard link share the
// file's identity, and another file does not. It reports the type after following a symlink.
static void test_stat_identifies_files(void) {
  char root[] = "/tmp/fram-os-stat.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "dir/a.md", S("a"));
  contract_write(&c, "b.md", S("b"));
  contract_symlink(&c, "dir/a.md", "link.md");
  contract_link(&c, "dir/a.md", "twin.md");
  contract_symlink(&c, "dir", "dir-link");
  contract_write(&c, "other/target", S("t"));
  struct Log log = test_log();
  TEST_CHECK(os_mkdirs(c.os, contract_path(&c, "sub"), S("failed to create fixture"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  contract_symlink(&c, "../other/target", "sub/link");

  const struct OsIdentity a = contract_identity(&c, "dir/a.md");
  TEST_CHECK(is_identity_equal(a, contract_identity(&c, "./dir/a.md")));
  TEST_CHECK(is_identity_equal(a, contract_identity(&c, "dir/../dir/a.md")));
  TEST_CHECK(is_identity_equal(a, contract_identity(&c, "link.md")));
  TEST_CHECK(is_identity_equal(a, contract_identity(&c, "twin.md")));
  TEST_CHECK(is_identity_equal(a, contract_identity(&c, "dir-link/a.md")));
  TEST_CHECK(!is_identity_equal(a, contract_identity(&c, "b.md")));
  TEST_CHECK(is_identity_equal(contract_identity(&c, "dir"), contract_identity(&c, "dir-link")));
  TEST_CHECK(
      is_identity_equal(contract_identity(&c, "other/target"), contract_identity(&c, "sub/link")));

  struct OsStat st;
  TEST_CHECK(os_stat(c.os, contract_path(&c, "link.md"), &st) == 0 && !st.is_dir);
  TEST_CHECK(os_stat(c.os, contract_path(&c, "dir-link"), &st) == 0 && st.is_dir);
  contract_end(&c);
}

// A path that cannot be inspected fails with `errno` set and the caller's struct left alone: a
// missing file, a dangling symlink, a path through a file, and a symlink cycle.
static void test_stat_rejects_what_names_nothing(void) {
  char root[] = "/tmp/fram-os-stat-fail.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "file", S("x"));
  contract_symlink(&c, "missing", "dangling");
  contract_symlink(&c, "loop-b", "loop-a");
  contract_symlink(&c, "loop-a", "loop-b");
  static const struct {
    const char* path;
    int error_number;
  } cases[] = {
      {"missing.md", ENOENT}, {"dangling", ENOENT}, {"file/x", ENOTDIR}, {"loop-a", ELOOP}};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i].path);
    struct OsStat unwritten = {.identity = {.device = 7, .inode = 11}};
    errno = 0;
    TEST_CHECK(os_stat(c.os, contract_path(&c, cases[i].path), &unwritten) == -1);
    TEST_CHECK(errno == cases[i].error_number);
    TEST_CHECK(unwritten.identity.device == 7 && unwritten.identity.inode == 11);
  }
  contract_end(&c);
}

// `os_print` writes each text whole and in call order to the stream it names, and an empty text
// writes nothing. On the POSIX host both streams are captured at their descriptors, and the
// in-memory host keeps what it was given.
static void test_print_writes_each_stream_in_order(void) {
  char root[] = "/tmp/fram-os-print.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  int print_a_rc = -1;
  int print_err_rc = -1;
  int print_empty_rc = -1;
  int print_bc_rc = -1;
  struct Str out_text = {0};
  struct Str err_text = {0};
#if !FRAM_TEST_OS_MEMORY
  struct StreamCapture out;
  struct StreamCapture err;
  char out_buffer[TEST_PRINTED_SIZE];
  char err_buffer[TEST_PRINTED_SIZE];
  if (capture_begin(stdout, &out) != 0) {
    goto cleanup;
  }
  if (capture_begin(stderr, &err) != 0) {
    (void)capture_end(&out, out_buffer, sizeof(out_buffer));
    goto cleanup;
  }
#endif
  print_a_rc = os_print(c.os, 1, S("a"));
  print_err_rc = os_print(c.os, 2, S("err\n"));
  print_empty_rc = os_print(c.os, 1, S(""));
  print_bc_rc = os_print(c.os, 1, S("bc\n"));
#if FRAM_TEST_OS_MEMORY
  out_text = os_memory_printed(c.os, 1);
  err_text = os_memory_printed(c.os, 2);
#else
  TEST_CHECK(capture_end(&err, err_buffer, sizeof(err_buffer)) == 0);
  TEST_CHECK(capture_end(&out, out_buffer, sizeof(out_buffer)) == 0);
  out_text = str_from_cstr(out_buffer);
  err_text = str_from_cstr(err_buffer);
#endif
  TEST_CHECK(print_a_rc == 0 && print_err_rc == 0 && print_empty_rc == 0 && print_bc_rc == 0);
  TEST_CHECK(str_eq(out_text, S("abc\n")));
  TEST_CHECK(str_eq(err_text, S("err\n")));

#if !FRAM_TEST_OS_MEMORY
cleanup:
#endif
  contract_end(&c);
}

TEST_LIST = {
    {"read accepts empty and file at limit", test_read_accepts_empty_and_file_at_limit},
    {"read accepts embedded nul", test_read_accepts_embedded_nul},
    {"read and write accept path below path max", test_read_and_write_accept_path_below_path_max},
    {"read follows symlinks and rejects cycle", test_read_follows_symlinks_and_rejects_cycle},
    {"read follows symlink chain up to system limit",
     test_read_follows_symlink_chain_up_to_system_limit},
    {"read rejects oversize", test_read_rejects_oversize},
    {"read checks size of extended file", test_read_checks_size_of_extended_file},
    {"read rejects missing and directory", test_read_rejects_missing_and_directory},
    {"read rejects path with nul and path at path max",
     test_read_rejects_path_with_nul_and_path_at_path_max},
    {"trailing slash requires directory", test_trailing_slash_requires_directory},
    {"write creates parents and round trips", test_write_creates_parents_and_round_trips},
    {"write overwrites in place", test_write_overwrites_in_place},
    {"write copies file source", test_write_copies_file_source},
    {"write follows symlink for every source", test_write_follows_symlink_for_every_source},
    {"write accepts name at name max", test_write_accepts_name_at_name_max},
    {"write rejects file parent and dir target", test_write_rejects_file_parent_and_dir_target},
    {"write and mkdirs reject path with nul", test_write_and_mkdirs_reject_path_with_nul},
    {"write rejects missing and directory source", test_write_rejects_missing_and_directory_source},
    {"write rejects dir target for every source", test_write_rejects_dir_target_for_every_source},
    {"mkdirs creates nested and is idempotent", test_mkdirs_creates_nested_and_is_idempotent},
    {"mkdirs rejects file and dangling components",
     test_mkdirs_rejects_file_and_dangling_components},
    {"list classifies entries and skips links to nothing",
     test_list_classifies_entries_and_skips_links_to_nothing},
    {"list rejects missing and file", test_list_rejects_missing_and_file},
    {"list marks link through file failed", test_list_marks_link_through_file_failed},
    {"stat reports size and write order", test_stat_reports_size_and_write_order},
    {"stat identifies files", test_stat_identifies_files},
    {"stat rejects what names nothing", test_stat_rejects_what_names_nothing},
    {"print writes each stream in order", test_print_writes_each_stream_in_order},
    {NULL, NULL},
};
