// `nanosleep` is POSIX rather than C17, so a strict `-std=c17` compile hides it unless a
// feature-test macro asks for it: `_DEFAULT_SOURCE` on glibc and musl, `_DARWIN_C_SOURCE` on macOS.
// Both must precede every `#include`.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "runtime/proc.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/** Descriptors the close-on-exec test inspects, from 0 up to but not including this one. */
enum { PROC_TEST_FD_COUNT = 256 };

/**
 * Shell script that prints every descriptor from 3 up to `PROC_TEST_FD_COUNT` the shell has open,
 * one per line. `test -e` is a builtin, so the shell opens nothing of its own to answer.
 */
#define PROC_TEST_LIST_FDS \
  "i=3; while [ $i -lt 256 ]; do if [ -e /dev/fd/$i ]; then echo $i; fi; i=$((i+1)); done"

/** One worker of the close-on-exec test: the child it runs, and what that child reported. */
struct ProcWorker {
  /** Shell script the worker's child runs. */
  const char* script;

  /** Milliseconds the worker waits before it spawns, so the other worker's child is running. */
  long delay_ms;

  /** Result of the worker's `proc_run`. */
  int rc;

  /** The child's standard output: the descriptors it had open. */
  struct Str output;

  /** Arena that owns `output`. */
  struct Arena arena;

  /** Log of the worker's `proc_run`. */
  struct Log log;

  /** Whether the worker's `proc_run` has returned. Set by the worker, read by the other. */
  atomic_bool is_done;

  /** Whether the other worker's `proc_run` had returned when this one returned. */
  bool is_other_done_first;

  /** The other worker. */
  struct ProcWorker* other;
};

/**
 * @brief Runs one worker's child through `proc_run`. The thread body of the close-on-exec test.
 *
 * @param arg `struct ProcWorker*` to run. Must not be `NULL`.
 * @return `NULL`.
 */
static void* run_worker(void* arg) __attribute__((nonnull(1)));

/**
 * @brief Records which descriptors this process has open, below `PROC_TEST_FD_COUNT`.
 *
 * @param is_open_out One flag per descriptor, set when it is open. Must not be `NULL`.
 */
static void record_open_fds(bool is_open_out[static PROC_TEST_FD_COUNT]);

/**
 * @brief Reports whether every descriptor a child listed was already open in this process before
 *        either worker started.
 *
 * A descriptor the test runner passed down is allowed, because CTest or a make jobserver can pass
 * descriptors of its own. Any other one belongs to the other worker.
 *
 * @param listed  One descriptor number per line, as `PROC_TEST_LIST_FDS` prints them.
 * @param is_open One flag per descriptor that was open before the workers started. Must not be
 *                `NULL`.
 * @return `true` when no listed descriptor is new.
 */
static bool is_inherited_only(struct Str listed, const bool is_open[static PROC_TEST_FD_COUNT]);

static void* run_worker(void* arg) {
  struct ProcWorker* worker = arg;
  const struct timespec delay = {.tv_sec = 0, .tv_nsec = worker->delay_ms * 1000000L};
  (void)nanosleep(&delay, NULL);
  const char* const argv[] = {"/bin/sh", "-c", worker->script, NULL};
  worker->rc = proc_run(argv, &worker->arena, &worker->output, S("failed to list descriptors"),
                        (struct Str){0}, &worker->log);
  worker->is_other_done_first = atomic_load(&worker->other->is_done);
  atomic_store(&worker->is_done, true);
  return NULL;
}

static void record_open_fds(bool is_open_out[static PROC_TEST_FD_COUNT]) {
  for (int fd = 0; fd < PROC_TEST_FD_COUNT; fd++) {
    is_open_out[fd] = fcntl(fd, F_GETFD) != -1;
  }
}

static bool is_inherited_only(struct Str listed, const bool is_open[static PROC_TEST_FD_COUNT]) {
  bool is_inherited = true;
  while (listed.len > 0) {
    const struct StrCut line = str_cut(listed, '\n');
    int fd = 0;
    for (ptrdiff_t i = 0; i < line.head.len && fd < PROC_TEST_FD_COUNT; i++) {
      fd = fd * 10 + (line.head.data[i] - '0');
    }
    if (fd >= PROC_TEST_FD_COUNT || !is_open[fd]) {
      TEST_MSG("descriptor %d was not open before the workers started", fd);
      is_inherited = false;
    }
    listed = line.tail;
  }
  return is_inherited;
}

// Arguments are passed directly without shell interpretation, so metacharacters remain literal.
static void test_run_captures_literal_argument(void) {
  const char* const argv[] = {"/bin/echo", "name with spaces;$(false)", NULL};
  struct Arena arena = test_arena();
  struct Str output = {0};
  struct Log log = test_log();
  TEST_CHECK(proc_run(argv, &arena, &output, S("failed to echo"), (struct Str){0}, &log) == 0);
  TEST_CHECK(str_eq(output, S("name with spaces;$(false)\n")));
  TEST_CHECK(test_log_is_empty(&log));
}

// The output counts every byte the child printed, so a `NUL` in it does not end the output early.
static void test_run_captures_output_past_nul(void) {
  const char* const argv[] = {"/bin/sh", "-c", "printf '12.5\\000junk'", NULL};
  struct Arena arena = test_arena();
  struct Str output = {0};
  struct Log log = test_log();
  TEST_CHECK(proc_run(argv, &arena, &output, S("failed to print"), (struct Str){0}, &log) == 0);
  TEST_CHECK(str_eq(output, (struct Str){"12.5\0junk", 9}));
}

// More than one pipe capacity is written to both streams. Reading one stream to EOF before the
// other would deadlock here; polling and draining them together completes and retains all stdout.
static void test_run_drains_stdout_and_stderr_together(void) {
  const char* const argv[] = {
      "/bin/sh", "-c", "i=0; while [ $i -lt 40000 ]; do printf o; printf e >&2; i=$((i+1)); done",
      NULL};
  struct Arena arena = test_arena();
  struct Str output = {0};
  struct Log log = test_log();
  TEST_CHECK(proc_run(argv, &arena, &output, S("failed to print"), (struct Str){0}, &log) == 0);
  TEST_CHECK(output.len == 40000 && output.data[0] == 'o' && output.data[39999] == 'o');
  TEST_CHECK(test_log_is_empty(&log));
}

// Two workers run a child each at the same time. Neither child holds a descriptor of the other
// worker, so the quick child's worker returns while the slow child still runs.
static void test_run_keeps_workers_descriptors_apart(void) {
  bool is_open[PROC_TEST_FD_COUNT];
  record_open_fds(is_open);
  struct ProcWorker slow = {
      .script = "sleep 1; " PROC_TEST_LIST_FDS,
      .arena = test_arena(),
      .log = test_log(),
  };
  struct ProcWorker quick = {
      .script = PROC_TEST_LIST_FDS,
      .delay_ms = 200,
      .arena = test_arena(),
      .log = test_log(),
  };
  atomic_init(&slow.is_done, false);
  atomic_init(&quick.is_done, false);
  slow.other = &quick;
  quick.other = &slow;
  pthread_t slow_thread;
  pthread_t quick_thread;
  TEST_ASSERT(pthread_create(&slow_thread, NULL, run_worker, &slow) == 0);
  if (!TEST_CHECK(pthread_create(&quick_thread, NULL, run_worker, &quick) == 0)) {
    (void)pthread_join(slow_thread, NULL);
    return;
  }
  (void)pthread_join(quick_thread, NULL);
  (void)pthread_join(slow_thread, NULL);

  TEST_CHECK(slow.rc == 0);
  TEST_CHECK(quick.rc == 0);
  TEST_CHECK(test_log_is_empty(&slow.log));
  TEST_CHECK(test_log_is_empty(&quick.log));
  TEST_CHECK(is_inherited_only(slow.output, is_open));
  TEST_CHECK(is_inherited_only(quick.output, is_open));
  TEST_CHECK(!quick.is_other_done_first);
}

// A nonzero exit reports the caller's operation, the status, the child's standard error and the
// file the child worked on.
static void test_run_reports_failed_exit(void) {
  const char* const argv[] = {"/bin/sh", "-c", "printf 'specific failure' >&2; exit 7", NULL};
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_CHECK(proc_run(argv, &arena, NULL, S("failed to probe media"), S("a.mp4"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: process exited with status 7: specific failure "
                         "('a.mp4')\n"));
}

// Standard error longer than the tail keeps its newest bytes, so the child's final line, which
// usually names the cause, survives and the dropped front is marked. The tail's lines are joined by
// `; `, so the failure is one line of the log.
static void test_run_reports_newest_stderr_when_it_overflows(void) {
  const char* const argv[] = {"/bin/sh", "-c",
                              "i=0; while [ $i -lt 400 ]; do echo \"warning: line $i\" >&2; "
                              "i=$((i+1)); done; echo 'FATAL: the actual cause' >&2; exit 1",
                              NULL};
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_CHECK(proc_run(argv, &arena, NULL, S("failed to probe media"), (struct Str){0}, &log) == -1);
  const struct Str text = buf_str(&log.buf);
  TEST_CHECK(str_startswith(text, S("failed to probe media: process exited with status 1: ...")));
  TEST_CHECK(str_endswith(text, S("warning: line 399; FATAL: the actual cause\n")));
  TEST_CHECK(str_cut(text, '\n').tail.len == 0);
  TEST_MSG("log: '%.*s'", STR_FMT(text));
}

// A `NUL` byte in standard error is shown as `?`, so the message does not end early at it.
static void test_run_shows_nul_in_stderr(void) {
  const char* const argv[] = {"/bin/sh", "-c", "printf 'bad\\000byte\\n' >&2; exit 1", NULL};
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_CHECK(proc_run(argv, &arena, NULL, S("failed to probe media"), S("a.mp4"), &log) == -1);
  TEST_CHECK(test_log_is_one(
      &log, "failed to probe media: process exited with status 1: bad?byte ('a.mp4')"));
}

// A missing executable receives the actionable program-not-found diagnostic rather than a raw
// `posix_spawn` error number.
static void test_run_reports_missing_program(void) {
  const char* const argv[] = {"fram-definitely-not-a-program", NULL};
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_CHECK(
      proc_run(argv, &arena, NULL, S("video sources require ffmpeg"), (struct Str){0}, &log) == -1);
  TEST_CHECK(
      test_log_is_one(&log,
                      "video sources require ffmpeg: program 'fram-definitely-not-a-program' "
                      "not found"));

  // A file the child would have worked on follows the program in its own parentheses.
  log = test_log();
  TEST_CHECK(proc_run(argv, &arena, NULL, S("failed to probe media"), S("a.mp4"), &log) == -1);
  TEST_CHECK(test_log_is_one(
      &log, "failed to probe media: program 'fram-definitely-not-a-program' not found ('a.mp4')"));
}

// Standard output past `PROC_OUTPUT_LEN_MAX` fails once the pipes are fully drained, and the
// message names how much the child wrote.
static void test_run_rejects_output_past_limit(void) {
  const char* const argv[] = {"/bin/sh", "-c", "dd if=/dev/zero bs=1000 count=70 2>/dev/null",
                              NULL};
  struct Arena arena = test_arena();
  struct Str output = {0};
  struct Log log = test_log();
  TEST_CHECK(proc_run(argv, &arena, &output, S("failed to probe media"), S("a.mp4"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: process output exceeds max output length (%d "
                         "bytes) at 70000 bytes ('a.mp4')\n",
                         PROC_OUTPUT_LEN_MAX));
  TEST_CHECK(output.len == 0);
}

TEST_LIST = {
    {"run captures literal argument", test_run_captures_literal_argument},
    {"run captures output past nul", test_run_captures_output_past_nul},
    {"run drains stdout and stderr together", test_run_drains_stdout_and_stderr_together},
    {"run keeps workers descriptors apart", test_run_keeps_workers_descriptors_apart},
    {"run reports failed exit", test_run_reports_failed_exit},
    {"run reports newest stderr when it overflows",
     test_run_reports_newest_stderr_when_it_overflows},
    {"run shows nul in stderr", test_run_shows_nul_in_stderr},
    {"run reports missing program", test_run_reports_missing_program},
    {"run rejects output past limit", test_run_rejects_output_past_limit},
    {NULL, NULL},
};
