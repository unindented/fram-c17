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
#include "runtime/os.h"
#include "runtime/walk.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few bytes. */
enum { TEST_FILE_LEN_MAX = 1024 };

/**
 * @brief Joins a relative path onto a fixture directory, keeping a terminator after it.
 *
 * The terminator lets a test hand `data` straight to a system call that sets up the fixture, while
 * the `fs` calls under test read the counted string.
 *
 * @param dir           Terminated fixture directory. Must not be `NULL`.
 * @param relative_path Terminated path below `dir`. Must not be `NULL`.
 * @param arena         Arena that owns the joined path. Must not be `NULL`.
 * @return The joined path, its `data` terminated.
 */
static struct Str fixture_path(const char* dir, const char* relative_path, struct Arena* arena) {
  const struct Str path = path_join(str_from_cstr(dir), str_from_cstr(relative_path), arena);
  return (struct Str){str_cstr(path, arena), path.len};
}

/**
 * @brief Returns the scratch arena every filesystem call in this file runs with.
 *
 * One reservation serves every call. A scratch arena passes by value, so whatever a call allocates
 * in it is dead once the call returns, and the next call may reuse the same bytes. No test holds a
 * pointer into it, and no test passes it as a long-lived arena.
 *
 * @return An arena over the shared scratch block.
 */
static struct Arena scratch_arena(void) {
  static struct Arena scratch;
  if (scratch.beg == NULL) {
    scratch = test_arena();
  }
  return scratch;
}

/**
 * @brief Lists files under `root_dir` matching one case-sensitive suffix.
 *
 * Adapts `walk_files_with_suffixes` on the POSIX host to the single-suffix shape the tests here
 * need.
 *
 * @param paths    Path list that receives the matching paths. Must not be `NULL`.
 * @param root_dir Directory tree to walk.
 * @param suffix   Filename suffix to match, such as `.md`.
 * @param perm     Arena that owns the listed paths. Must not be `NULL`.
 * @param log      Log that receives a failure, led by the operation `op`. Must not be `NULL`.
 * @return `0` on success, or `-1` on a directory or entry failure.
 */
static int list_files(struct PathList* paths,
                      struct Str root_dir,
                      struct Str suffix,
                      struct Arena* perm,
                      struct Log* log) {
  return walk_files_with_suffixes(paths, root_dir, S(""), &suffix, 1, false, os_posix(), perm,
                                  scratch_arena(), S("op"), log);
}

/**
 * Signals `count_interrupt` handles before it stops the interrupted write's reader. At one signal
 * each millisecond that is about ten seconds, far past the tenth of a second the write takes.
 */
enum { INTERRUPT_COUNT_MAX = 10000 };

/** Signals `count_interrupt` has handled since a test last reset it. */
static volatile sig_atomic_t interrupt_count = 0;

/** Process `count_interrupt` kills once it has handled `INTERRUPT_COUNT_MAX` signals, or `0`. */
static volatile sig_atomic_t interrupt_reader_pid = 0;

/** Read end `count_interrupt` closes once it has handled `INTERRUPT_COUNT_MAX` signals, or `-1`. */
static volatile sig_atomic_t interrupt_hold_fd = -1;

/**
 * @brief Counts one delivered signal. Past `INTERRUPT_COUNT_MAX`, it ends the write it interrupts.
 *
 * Installed without `SA_RESTART`, so each delivery interrupts a blocked system call. A write to a
 * FIFO blocks for as long as any read end is open, so a reader that died or never started would
 * hang the test. Killing the reader and closing the held read end leaves the FIFO with no reader,
 * so the write fails with `EPIPE` and the test fails instead. `kill` and `close` are safe to call
 * from a signal handler.
 *
 * @param signal_number Signal delivered. Unused.
 */
static void count_interrupt(int signal_number) {
  (void)signal_number;
  interrupt_count++;
  if (interrupt_count == INTERRUPT_COUNT_MAX) {
    if (interrupt_reader_pid > 0) {
      (void)kill((pid_t)interrupt_reader_pid, SIGKILL);
    }
    if (interrupt_hold_fd >= 0) {
      (void)close(interrupt_hold_fd);
      interrupt_hold_fd = -1;
    }
  }
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

// These tests check what only a real filesystem has: FIFOs, modes, permission bits, interrupted
// writes, a file size limit and the open-file limit. The contract tests in `test_os.c` check the
// rest of `os.h` against this host and the in-memory one.
//
// Every call here runs with the operation `op`, so an expected failure reads `op: <cause>`. A
// fixture write that fails is a broken test rather than behavior under test, and it shows up as a
// test log that is not empty where the test checks for one.

// A FIFO is rejected as not a regular file instead of blocking the open until a writer opens it,
// leaving the outputs untouched.
static void test_read_rejects_fifo(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/fram-fs-read-fifo.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str fifo = fixture_path(root_dir, "fifo.md", &arena);
  const struct Str sentinel = S("unchanged");
  struct Str file_data = sentinel;
  if (!TEST_CHECK(mkfifo(fifo.data, 0600) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(os_read(os_posix(), fifo, TEST_FILE_LEN_MAX, &arena, &file_data, S("op"), &log) == -1);
  TEST_CHECK(file_data.data == sentinel.data && file_data.len == sentinel.len);
  TEST_CHECK(test_log_is(&log, "op: not a regular file ('%.*s')\n", STR_FMT(fifo)));

cleanup:
  remove_fixture_tree(root_dir);
}

// `os_write` creates a new file with `0666` reduced by the process umask, from every source, so a
// restrictive umask is not discarded and a copy does not take its source's mode. Overwriting an
// existing file keeps that file's mode, because the write goes through the file.
static void test_write_applies_umask_and_keeps_existing_mode(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/fram-fs-mode.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str created = fixture_path(root_dir, "created.html", &arena);
  struct Str existing = fixture_path(root_dir, "existing.html", &arena);
  struct Str source = fixture_path(root_dir, "source.css", &arena);
  struct Str copied = fixture_path(root_dir, "copied.css", &arena);
  const struct OsSource copy = {.kind = OS_SOURCE_FILE, .file_path = source};

  // A restrictive umask is the case a hardcoded mode would discard. Restore it before asserting, so
  // a failure cannot leak the tightened value into later tests.
  const mode_t umask_before = umask(077);
  TEST_CHECK(os_write(os_posix(), created, (struct OsSource){.bytes = S("x")}, S("op"), &log) == 0);
  TEST_CHECK(os_write(os_posix(), existing, (struct OsSource){.bytes = S("x")}, S("op"), &log) ==
             0);
  TEST_CHECK(chmod(existing.data, 0640) == 0);
  TEST_CHECK(os_write(os_posix(), existing, (struct OsSource){.bytes = S("y")}, S("op"), &log) ==
             0);
  // The source is given wide permissions the copy must not inherit.
  TEST_CHECK(os_write(os_posix(), source, (struct OsSource){.bytes = S("body{}")}, S("op"), &log) ==
             0);
  TEST_CHECK(chmod(source.data, 0777) == 0);
  TEST_CHECK(os_write(os_posix(), copied, copy, S("op"), &log) == 0);
  (void)umask(umask_before);

  TEST_CHECK(test_log_is_empty(&log));
  const mode_t expected = (mode_t)(0666 & ~077);
  struct stat st;
  TEST_CHECK(stat(created.data, &st) == 0 && (st.st_mode & 07777) == expected);
  TEST_CHECK(stat(existing.data, &st) == 0 && (st.st_mode & 07777) == 0640);
  TEST_CHECK(stat(copied.data, &st) == 0 && (st.st_mode & 07777) == expected);
  remove_fixture_tree(root_dir);
}

// `os_write` finishes a write that signals interrupt instead of failing with `EINTR`. A FIFO whose
// reader starts late stands in for a slow file, because a write to a local regular file is not
// interrupted. The handler is installed without `SA_RESTART`, so each signal does interrupt the
// blocked `write`. Two ways this could hang are closed: a write blocked on a reader that is gone
// (the handler stops it, see `count_interrupt`), and a reader blocked in its `open` because the
// write failed before it opened the FIFO (the reader is then stopped, not awaited).
static void test_write_retries_interrupted_write(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/fram-fs-write-eintr.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str fifo = fixture_path(root_dir, "fifo.html", &arena);
  // Larger than any pipe buffer, so the write blocks until the reader drains it.
  static char data[1024 * 1024];
  int hold_fd = -1;
  pid_t reader = -1;
  struct sigaction action = {.sa_handler = count_interrupt};
  struct sigaction alarm_action_before;
  struct sigaction pipe_action_ignore = {.sa_handler = SIG_IGN};
  struct sigaction pipe_action_before;
  const struct itimerval every_millisecond = {.it_interval = {.tv_usec = 1000},
                                              .it_value = {.tv_usec = 1000}};
  const struct itimerval stopped = {0};
  int rc = -1;
  int status = 0;
  if (!TEST_CHECK(mkfifo(fifo.data, 0600) == 0)) {
    goto cleanup;
  }
  // A read end held open lets the write's `open` return at once, so only its `write` blocks.
  hold_fd = open(fifo.data, O_RDONLY | O_NONBLOCK);
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
    _exit(drain_fifo(fifo.data, data, sizeof(data)) == 0 ? 0 : 1);
  }

  (void)sigemptyset(&action.sa_mask);
  (void)sigemptyset(&pipe_action_ignore.sa_mask);
  // A write with no reader left fails with `EPIPE` rather than ending the process.
  if (!TEST_CHECK(sigaction(SIGPIPE, &pipe_action_ignore, &pipe_action_before) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(sigaction(SIGALRM, &action, &alarm_action_before) == 0)) {
    (void)sigaction(SIGPIPE, &pipe_action_before, NULL);
    goto cleanup;
  }
  interrupt_count = 0;
  interrupt_reader_pid = reader;
  interrupt_hold_fd = hold_fd;
  if (!TEST_CHECK(setitimer(ITIMER_REAL, &every_millisecond, NULL) == 0)) {
    (void)sigaction(SIGALRM, &alarm_action_before, NULL);
    (void)sigaction(SIGPIPE, &pipe_action_before, NULL);
    goto cleanup;
  }
  rc = os_write(os_posix(), fifo, (struct OsSource){.bytes = (struct Str){data, sizeof(data)}},
                S("op"), &log);
  // Stop the timer before restoring the previous action, so no signal reaches the default one.
  (void)setitimer(ITIMER_REAL, &stopped, NULL);
  (void)sigaction(SIGALRM, &alarm_action_before, NULL);
  (void)sigaction(SIGPIPE, &pipe_action_before, NULL);
  // The handler may have closed the held read end. It owns it from here.
  hold_fd = interrupt_hold_fd;
  interrupt_reader_pid = 0;
  interrupt_hold_fd = -1;
  // A write that failed may have failed before it opened the FIFO, and then the reader still waits
  // in its `open` for a writer. Only a reader that got the whole write is awaited.
  if (rc == 0) {
    TEST_CHECK(waitpid(reader, &status, 0) == reader);
    reader = -1;
  }

  TEST_CHECK(rc == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(interrupt_count > 0);
  TEST_CHECK(rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);

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
  remove_fixture_tree(root_dir);
}

// A FIFO file source is rejected as not a regular file instead of blocking the write until a writer
// opens it, and the destination is never created.
static void test_write_rejects_fifo_source(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/fram-fs-copy-fifo.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str fifo = fixture_path(root_dir, "source.mp4", &arena);
  struct Str dest = fixture_path(root_dir, "copy.mp4", &arena);
  if (!TEST_CHECK(mkfifo(fifo.data, 0600) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(os_write(os_posix(), dest,
                      (struct OsSource){.kind = OS_SOURCE_FILE, .file_path = fifo}, S("op"),
                      &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: not a regular file ('%.*s')\n", STR_FMT(fifo)));
  struct OsStat st;
  TEST_CHECK(os_stat(os_posix(), dest, &st) == -1);

cleanup:
  remove_fixture_tree(root_dir);
}

// A write that the system cuts short, here at a file size limit, fails with the system's message
// naming the destination, and leaves the bytes below the limit and nothing beside the file.
static void test_write_reports_destination_when_write_fails(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/fram-fs-write-fsize.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str output = fixture_path(root_dir, "out.jpg", &arena);
  TEST_CHECK(os_write(os_posix(), output, (struct OsSource){.bytes = S("old")}, S("op"), &log) ==
             0);
  struct rlimit limit;
  struct sigaction fsize_action_ignore = {.sa_handler = SIG_IGN};
  struct sigaction fsize_action_before;
  (void)sigemptyset(&fsize_action_ignore.sa_mask);
  // A write past the limit fails with `EFBIG` once `SIGXFSZ` is ignored, after the bytes below it.
  struct rlimit lowered = {.rlim_cur = 4};
  int rc = 0;
  if (TEST_CHECK(getrlimit(RLIMIT_FSIZE, &limit) == 0) &&
      TEST_CHECK(sigaction(SIGXFSZ, &fsize_action_ignore, &fsize_action_before) == 0)) {
    lowered.rlim_max = limit.rlim_max;
    if (TEST_CHECK(setrlimit(RLIMIT_FSIZE, &lowered) == 0)) {
      rc = os_write(os_posix(), output, (struct OsSource){.bytes = S("longer than four")}, S("op"),
                    &log);
      // Restore the limit before asserting, so a failure cannot cut later tests' files short.
      (void)setrlimit(RLIMIT_FSIZE, &limit);
    }
    (void)sigaction(SIGXFSZ, &fsize_action_before, NULL);
  }

  TEST_CHECK(rc == -1);
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(EFBIG), STR_FMT(output)));
  struct Str data = {0};
  TEST_CHECK(os_read(os_posix(), output, TEST_FILE_LEN_MAX, &arena, &data, S("op"), &log) == 0);
  TEST_CHECK(str_eq(data, S("long")));
  struct OsEntry* entries = NULL;
  ptrdiff_t count = 0;
  TEST_CHECK(
      os_list(os_posix(), str_from_cstr(root_dir), &arena, &entries, &count, S("op"), &log) == 0);
  TEST_CHECK(count == 1);
  remove_fixture_tree(root_dir);
}

// A write to an existing file in a directory that may not be written succeeds, because it goes
// through the file, as a rebuild of a derivative in a read-only directory does. A new file there
// fails, naming the destination.
static void test_write_goes_through_existing_file_in_read_only_dir(void) {
  struct Log log = test_log();
  // Root bypasses the permission bits, so the new file would be created.
  if (geteuid() == 0) {
    return;
  }

  char root_dir[] = "/tmp/fram-fs-write-read-only.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str sealed_dir = fixture_path(root_dir, "sealed", &arena);
  struct Str existing = fixture_path(root_dir, "sealed/a-jpg-s.jpg", &arena);
  struct Str created = fixture_path(root_dir, "sealed/a-jpg-m.jpg", &arena);
  TEST_CHECK(os_write(os_posix(), existing, (struct OsSource){.bytes = S("old")}, S("op"), &log) ==
             0);
  TEST_CHECK(chmod(sealed_dir.data, 0500) == 0);
  const int existing_rc =
      os_write(os_posix(), existing, (struct OsSource){.bytes = S("new")}, S("op"), &log);
  struct Log created_log = test_log();
  const int created_rc =
      os_write(os_posix(), created, (struct OsSource){.bytes = S("new")}, S("op"), &created_log);
  // Restore the mode before checking anything. The cleanup below cannot remove a directory it may
  // not write.
  (void)chmod(sealed_dir.data, 0700);

  TEST_CHECK(existing_rc == 0);
  TEST_CHECK(test_log_is_empty(&log));
  struct Str data = {0};
  TEST_CHECK(os_read(os_posix(), existing, TEST_FILE_LEN_MAX, &arena, &data, S("op"), &log) == 0);
  TEST_CHECK(str_eq(data, S("new")));
  TEST_CHECK(created_rc == -1);
  TEST_CHECK(test_log_is(&created_log, "op: %s ('%.*s')\n", strerror(EACCES), STR_FMT(created)));
  remove_fixture_tree(root_dir);
}

// `os_mkdirs` fails when `mkdir` itself is refused. The failure is neither the first-party type
// rejection nor the `EEXIST` fallthrough, which `test_os.c` covers, but the system error reported
// straight from the create. A parent that is readable but not writable is the way to produce it.
static void test_mkdirs_rejects_uncreatable_component(void) {
  struct Log log = test_log();
  // Root bypasses the permission bits, so `mkdir` would succeed and the assertions below would fail
  // for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }

  char root_dir[] = "/tmp/fram-fs-mkdir-refused.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str sealed_dir = fixture_path(root_dir, "sealed", &arena);
  struct Str child_dir = fixture_path(root_dir, "sealed/child", &arena);
  TEST_CHECK(os_mkdirs(os_posix(), sealed_dir, S("op"), &log) == 0);
  TEST_CHECK(chmod(sealed_dir.data, 0500) == 0);

  const int rc = os_mkdirs(os_posix(), child_dir, S("op"), &log);
  // Restore the mode before checking anything, as `test_list_rejects_unstatable_entry` does. The
  // cleanup below cannot remove a directory it may not write.
  (void)chmod(sealed_dir.data, 0700);

  TEST_CHECK(rc == -1);
  TEST_CHECK(test_log_is(&log, "op: cannot create directory: %s ('%.*s')\n", strerror(EACCES),
                         STR_FMT(child_dir)));
  remove_fixture_tree(root_dir);
}

// `os_list` leaves out a FIFO, which names no regular file or directory, and keeps the regular file
// beside it. `test_os.c` checks the rest of the classification against both hosts.
static void test_list_skips_fifo(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/fram-fs-list-fifo.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  TEST_CHECK(os_write(os_posix(), fixture_path(root_dir, "file.md", &arena),
                      (struct OsSource){.bytes = S("b")}, S("op"), &log) == 0);
  TEST_CHECK(mkfifo(fixture_path(root_dir, "fifo", &arena).data, 0600) == 0);

  struct OsEntry* entries = NULL;
  ptrdiff_t count = 0;
  TEST_CHECK(
      os_list(os_posix(), str_from_cstr(root_dir), &arena, &entries, &count, S("op"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(count == 1 && str_eq(entries[0].name, S("file.md")) &&
             entries[0].kind == OS_ENTRY_FILE);
  remove_fixture_tree(root_dir);
}

// An entry that exists but cannot be inspected fails the walk and names that entry, rather than
// being dropped while the walk still reports success. A directory that is readable but not
// searchable is the reachable way to produce it: `readdir` lists the entry, and `stat` on the entry
// then fails with `EACCES`.
static void test_list_rejects_unstatable_entry(void) {
  struct Log log = test_log();
  // Root bypasses the permission bits, so the sealed directory would be walked like any other and
  // the assertions below would fail for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }

  char root_dir[] = "/tmp/fram-fs-list-unstatable.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str sealed_dir = fixture_path(root_dir, "sealed", &arena);
  struct Str sealed_md = fixture_path(root_dir, "sealed/a.md", &arena);
  TEST_CHECK(
      os_write(os_posix(), sealed_md, (struct OsSource){.bytes = S("hidden")}, S("op"), &log) == 0);
  TEST_CHECK(chmod(sealed_dir.data, 0400) == 0);

  struct PathList paths = {0};
  const int rc = list_files(&paths, str_from_cstr(root_dir), S(".md"), &arena, &log);
  // Restore the mode before checking anything: neither the cleanup below nor the harness can remove
  // a directory it is not allowed to search.
  (void)chmod(sealed_dir.data, 0700);

  TEST_CHECK(rc == -1);
  TEST_CHECK(paths.count == 0);
  TEST_CHECK(test_log_is(&log, "op: cannot inspect entry: %s ('%.*s')\n", strerror(EACCES),
                         STR_FMT(sealed_md)));
  remove_fixture_tree(root_dir);
}

// An entry the listing could not inspect is reported when the sorted walk reaches it, on the real
// filesystem as on the in-memory host: of two such siblings, the first in byte order, and an error
// inside an earlier subdirectory before a later such sibling. A symlink through a regular file is
// such an entry, and needs no permission change.
static void test_list_reports_uninspectable_entries_in_order(void) {
  static const struct {
    const char* links[2][2];
    const char* reported;
  } cases[] = {
      {{{"../file.md/y", "site/b.md"}, {"../file.md/y", "site/a.md"}}, "site/a.md"},
      {{{"../file.md/y", "site/b.md"}, {"../../file.md/y", "site/a/z.md"}}, "site/a/z.md"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i].reported);
    char root_dir[] = "/tmp/fram-fs-list-order.XXXXXX";
    if (init_fixture_dir(root_dir) != 0) {
      return;
    }
    struct Arena arena = test_arena();
    struct Log log = test_log();
    const int fd = open(fixture_path(root_dir, "file.md", &arena).data, O_WRONLY | O_CREAT, 0600);
    TEST_CHECK(fd >= 0 && close(fd) == 0);
    TEST_CHECK(mkdir(fixture_path(root_dir, "site", &arena).data, 0700) == 0);
    TEST_CHECK(mkdir(fixture_path(root_dir, "site/a", &arena).data, 0700) == 0);
    for (ptrdiff_t j = 0; j < 2; j++) {
      const struct Str link = fixture_path(root_dir, cases[i].links[j][1], &arena);
      TEST_CHECK(symlink(cases[i].links[j][0], link.data) == 0);
    }
    struct PathList paths = {0};
    const struct Str site = fixture_path(root_dir, "site", &arena);
    TEST_CHECK(list_files(&paths, site, S(".md"), &arena, &log) == -1);
    const struct Str reported = fixture_path(root_dir, cases[i].reported, &arena);
    TEST_CHECK(test_log_is(&log, "op: cannot inspect entry: %s ('%.*s')\n", strerror(ENOTDIR),
                           STR_FMT(reported)));
    remove_fixture_tree(root_dir);
  }
}

// A tree deeper than the open-file limit is walked, because `os_list` closes each directory before
// the walk descends into its subdirectories rather than holding one stream open per level.
static void test_walk_descends_deeper_than_open_file_limit(void) {
  struct Log log = test_log();
  enum { TREE_DEPTH = 64, OPEN_FILE_LIMIT = 32 };
  char root_dir[] = "/tmp/fram-fs-list-deep.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str leaf_dir = fixture_path(root_dir, "d", &arena);
  for (size_t i = 1; i < TREE_DEPTH; i++) {
    leaf_dir = fixture_path(leaf_dir.data, "d", &arena);
  }
  const struct Str page = fixture_path(leaf_dir.data, "page.md", &arena);
  TEST_CHECK(os_write(os_posix(), page, (struct OsSource){.bytes = S("x")}, S("op"), &log) == 0);

  struct rlimit limit;
  struct rlimit lowered;
  struct PathList paths = {0};
  int rc = -1;
  if (!TEST_CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0)) {
    goto cleanup;
  }
  lowered = (struct rlimit){.rlim_cur = OPEN_FILE_LIMIT, .rlim_max = limit.rlim_max};
  if (!TEST_CHECK(setrlimit(RLIMIT_NOFILE, &lowered) == 0)) {
    goto cleanup;
  }
  rc = list_files(&paths, str_from_cstr(root_dir), S(".md"), &arena, &log);
  // Restore the limit before asserting, so a failure cannot starve later tests of descriptors.
  (void)setrlimit(RLIMIT_NOFILE, &limit);

  TEST_CHECK(rc == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(paths.count == 1 && str_eq(paths.items[0], page));

cleanup:
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"read rejects fifo", test_read_rejects_fifo},
    {"write applies umask and keeps existing mode",
     test_write_applies_umask_and_keeps_existing_mode},
    {"write retries interrupted write", test_write_retries_interrupted_write},
    {"write rejects fifo source", test_write_rejects_fifo_source},
    {"write reports destination when write fails", test_write_reports_destination_when_write_fails},
    {"write goes through existing file in read only dir",
     test_write_goes_through_existing_file_in_read_only_dir},
    {"mkdirs rejects uncreatable component", test_mkdirs_rejects_uncreatable_component},
    {"list skips fifo", test_list_skips_fifo},
    {"list rejects unstatable entry", test_list_rejects_unstatable_entry},
    {"list reports uninspectable entries in order",
     test_list_reports_uninspectable_entries_in_order},
    {"walk descends deeper than open file limit", test_walk_descends_deeper_than_open_file_limit},
    {NULL, NULL},
};
