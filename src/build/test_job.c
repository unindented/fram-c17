// `pthread_setattr_default_np` is a GNU extension, so glibc needs `_GNU_SOURCE` to declare it.
#define _GNU_SOURCE

#include <acutest.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build/job.h"
#include "core/error.h"
#include "os_memory.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * Most workers any test here requests, and so the number of worker arena pairs `run_jobs` makes.
 */
enum { TEST_WORKER_COUNT_MAX = 4 };

/** Number of jobs `test_run_hands_each_job_its_worker_arenas` runs. */
enum { ARENA_JOB_COUNT = 8 };

/** Number of jobs `test_run_prints_progress_lines_whole_and_in_order` runs. */
enum { PROGRESS_JOB_COUNT = 64 };

/** Shared state for the failing jobs below: how many of them fail. */
struct FailureContext {
  /** Number of leading jobs that fail. */
  ptrdiff_t failure_count;
};

/**
 * @brief Fails the first `failure_count` jobs, each with a message naming its own index.
 *
 * Distinct messages are what exercise the report cap itself rather than its deduplication.
 *
 * @param index    Job index this call owns.
 * @param arenas   Arenas of the worker running the job. Unused.
 * @param log      Log of the job.
 * @param userdata `struct FailureContext` holding the failure count.
 * @return `0` when the index is past the failing range, or `-1` after recording an error.
 */
static int fail_selected_job(ptrdiff_t index,
                             struct JobArenas* arenas,
                             struct Log* log,
                             void* userdata) {
  (void)arenas;
  const struct FailureContext* context = userdata;
  if (index >= context->failure_count) {
    return 0;
  }
  return log_error(log, "failure %td", index);
}

/**
 * @brief Fails every job with one identical message.
 *
 * This is the shape a systematic failure takes: one cause repeated across every job, which is what
 * deduplication has to collapse.
 *
 * @param index    Job index this call owns. Unused.
 * @param arenas   Arenas of the worker running the job. Unused.
 * @param log      Log of the job.
 * @param userdata Unused.
 * @return `-1` always, after recording the shared error.
 */
static int fail_with_one_message(ptrdiff_t index,
                                 struct JobArenas* arenas,
                                 struct Log* log,
                                 void* userdata) {
  (void)index;
  (void)arenas;
  (void)userdata;
  return log_error(log, "template 'page.html' could not be parsed");
}

/**
 * @brief Fails every job with one of `failure_count` causes, chosen by its index, and names the job
 *        after the cause, as a page render does.
 *
 * @param index    Job index this call owns.
 * @param arenas   Arenas of the worker running the job. Unused.
 * @param log      Log of the job.
 * @param userdata `struct FailureContext` holding the number of causes.
 * @return `-1` always, after recording the error.
 */
static int fail_with_attributed_cause(ptrdiff_t index,
                                      struct JobArenas* arenas,
                                      struct Log* log,
                                      void* userdata) {
  (void)arenas;
  const struct FailureContext* context = userdata;
  return log_error(log, "cause %td (while rendering 'content/%td.md')",
                   index % context->failure_count, index);
}

/**
 * @brief Records two errors naming its own index.
 *
 * @param index    Job index this call owns.
 * @param arenas   Arenas of the worker running the job. Unused.
 * @param log      Log of the job.
 * @param userdata Unused.
 * @return `-1` always, after recording both errors.
 */
static int fail_twice(ptrdiff_t index, struct JobArenas* arenas, struct Log* log, void* userdata) {
  (void)arenas;
  (void)userdata;
  (void)log_error(log, "job %td first", index);
  return log_error(log, "job %td second", index);
}

/**
 * @brief Fails without recording an error.
 *
 * @param index    Unused job index.
 * @param arenas   Unused worker arenas.
 * @param log      Unused log.
 * @param userdata Unused.
 * @return `-1` always.
 */
static int fail_silently(ptrdiff_t index,
                         struct JobArenas* arenas,
                         struct Log* log,
                         void* userdata) {
  (void)index;
  (void)arenas;
  (void)log;
  (void)userdata;
  return -1;
}

/**
 * @brief Succeeds without recording anything.
 *
 * @param index    Unused job index.
 * @param arenas   Unused worker arenas.
 * @param log      Unused log.
 * @param userdata Unused.
 * @return `0` always.
 */
static int succeed_job(ptrdiff_t index, struct JobArenas* arenas, struct Log* log, void* userdata) {
  (void)index;
  (void)arenas;
  (void)log;
  (void)userdata;
  return 0;
}

// glibc lets a test change the default thread attributes, so only there can a test make
// `pthread_create` fail on demand.
#ifdef __GLIBC__
/**
 * @brief Returns at once. The thread `thread_create_fail_begin` tries to start runs it.
 *
 * @param arg Unused.
 * @return `NULL`.
 */
static void* thread_return(void* arg) {
  (void)arg;
  return NULL;
}

/**
 * @brief Makes every later `pthread_create` with default attributes fail.
 *
 * Raises the default stack size past any address space, so no thread stack can be mapped. The
 * caller restores the defaults it saved with `pthread_setattr_default_np`.
 *
 * @return The error `pthread_create` now returns, or `0` when the defaults could not be changed or
 *         a thread still started.
 */
static int thread_create_fail_begin(void) {
  pthread_attr_t failing;
  if (pthread_getattr_default_np(&failing) != 0) {
    return 0;
  }
  const bool is_set = pthread_attr_setstacksize(&failing, SIZE_MAX / 4) == 0 &&
                      pthread_setattr_default_np(&failing) == 0;
  (void)pthread_attr_destroy(&failing);
  if (!is_set) {
    return 0;
  }
  pthread_t thread;
  const int error_number = pthread_create(&thread, NULL, thread_return, NULL);
  if (error_number == 0) {
    (void)pthread_join(thread, NULL);
  }
  return error_number;
}
#endif

/**
 * @brief Runs `job_fn` through `job_run` with fresh worker arenas and a fresh main scratch.
 *
 * Each worker gets a long-lived and a scratch arena of its own, and the main scratch is a separate
 * reservation, so no two arenas overlap.
 *
 * @param job_count    Number of job indexes to run.
 * @param worker_count Requested worker threads. At most `TEST_WORKER_COUNT_MAX`.
 * @param job_fn       Callback invoked for each index. Must not be `NULL`.
 * @param userdata     Opaque pointer forwarded to every `job_fn` call.
 * @param log          Log that receives the collected errors. Must not be `NULL`.
 * @return The result of `job_run`.
 */
static int run_jobs(ptrdiff_t job_count,
                    ptrdiff_t worker_count,
                    JobFn job_fn,
                    void* userdata,
                    struct Log* log) {
  struct JobArenas worker_arenas[TEST_WORKER_COUNT_MAX];
  for (ptrdiff_t i = 0; i < TEST_WORKER_COUNT_MAX; i++) {
    worker_arenas[i] = (struct JobArenas){.perm = test_arena(), .scratch = test_arena()};
  }
  return job_run(job_count, worker_count, worker_arenas, job_fn, userdata, S("rendering"),
                 S(" (while rendering '"), false, os_memory(), test_arena(), log);
}

/** What `record_arenas_job` records for each job, indexed by job. */
struct ArenaRecord {
  /** Worker arena pair each job received. */
  struct JobArenas* received[ARENA_JOB_COUNT];

  /** Log each job received. */
  struct Log* logs[ARENA_JOB_COUNT];

  /** First free byte of the arena of each job's log when the job started. */
  const char* log_starts[ARENA_JOB_COUNT];

  /** Thread that ran each job. */
  pthread_t threads[ARENA_JOB_COUNT];
};

/**
 * @brief Records the worker arenas and the log a job received, where the log's arena starts and the
 *        thread running it, and allocates from a copy of its scratch.
 *
 * The allocation writes to the worker's scratch, so under ThreadSanitizer two jobs that shared an
 * arena pair while running at once would show as a race.
 *
 * @param index    Job index this call owns, below `ARENA_JOB_COUNT`.
 * @param arenas   Arenas of the worker running the job.
 * @param log      Log of the job.
 * @param userdata `struct ArenaRecord` receiving the record.
 * @return `0` always.
 */
static int record_arenas_job(ptrdiff_t index,
                             struct JobArenas* arenas,
                             struct Log* log,
                             void* userdata) {
  struct ArenaRecord* record = userdata;
  record->received[index] = arenas;
  record->logs[index] = log;
  record->log_starts[index] = log->buf.arena->beg;
  record->threads[index] = pthread_self();
  struct Arena scratch = arenas->scratch;
  *arena_new(&scratch, 1, ptrdiff_t) = index;
  return 0;
}

// An empty phase and a phase whose jobs all succeed both succeed without an error.
static void test_run_accepts_empty_and_successful_sets(void) {
  struct Log log = test_log();
  TEST_CHECK(run_jobs(0, 2, succeed_job, NULL, &log) == 0);
  TEST_CHECK(run_jobs(3, 2, succeed_job, NULL, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(log.errors == 0);
}

// Each job receives one of the arena pairs the caller supplied, the one for the worker running it,
// so a job's allocations land in memory no concurrent job uses, and a log of its own whose arena is
// in the scratch the caller passed rather than in either of those.
static void test_run_hands_each_job_its_worker_arenas(void) {
  enum { WORKER_COUNT = 3 };
  struct JobArenas worker_arenas[WORKER_COUNT];
  for (ptrdiff_t i = 0; i < WORKER_COUNT; i++) {
    worker_arenas[i] = (struct JobArenas){.perm = test_arena(), .scratch = test_arena()};
  }
  const struct Arena scratch = test_arena();
  struct ArenaRecord record = {0};
  struct Log log = test_log();
  TEST_CHECK(job_run(ARENA_JOB_COUNT, WORKER_COUNT, worker_arenas, record_arenas_job, &record,
                     S("rendering"), (struct Str){0}, false, os_memory(), scratch, &log) == 0);
  for (ptrdiff_t i = 0; i < ARENA_JOB_COUNT; i++) {
    TEST_CHECK(record.received[i] >= worker_arenas &&
               record.received[i] < worker_arenas + WORKER_COUNT);
    TEST_CHECK(record.logs[i] != &log);
    TEST_CHECK(record.log_starts[i] >= scratch.beg && record.log_starts[i] < scratch.limit);
    for (ptrdiff_t j = i + 1; j < ARENA_JOB_COUNT; j++) {
      TEST_CHECK((record.received[i] == record.received[j]) ==
                 (pthread_equal(record.threads[i], record.threads[j]) != 0));
      TEST_CHECK(record.logs[i] != record.logs[j]);
    }
  }
  TEST_CHECK(test_log_is_empty(&log));
}

// A verbose run prints one progress line per finished job, each whole although several workers
// finish at once, in count order, and then one newline. The count is taken under the lock that
// keeps a line whole, so the phase ends on the full count.
static void test_run_prints_progress_lines_whole_and_in_order(void) {
  struct JobArenas worker_arenas[TEST_WORKER_COUNT_MAX];
  for (ptrdiff_t i = 0; i < TEST_WORKER_COUNT_MAX; i++) {
    worker_arenas[i] = (struct JobArenas){.perm = test_arena(), .scratch = test_arena()};
  }
  struct Log log = test_log();
  struct Os* os = os_memory();
  TEST_CHECK(job_run(PROGRESS_JOB_COUNT, TEST_WORKER_COUNT_MAX, worker_arenas, succeed_job, NULL,
                     S("rendering"), (struct Str){0}, true, os, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));

  struct Arena arena = test_arena();
  struct Buf expected = {.arena = &arena};
  for (ptrdiff_t i = 1; i <= PROGRESS_JOB_COUNT; i++) {
    buf_printf(&expected, "\rrendering %td/%d", i, PROGRESS_JOB_COUNT);
  }
  buf_printf(&expected, "\n");
  TEST_CHECK(os_memory_is_printed(os, 2, buf_cstr(&expected)));
}

// Every error a job records is kept, in the order it recorded them, and each one counts.
static void test_run_keeps_every_error_of_a_job(void) {
  struct Log log = test_log();
  TEST_CHECK(run_jobs(1, 1, fail_twice, NULL, &log) == -1);
  TEST_CHECK(test_log_is(&log, "job 0 first\njob 0 second\n"));
  TEST_CHECK(log.errors == 2);
}

// Failed jobs are reported in job order, each job's errors together, however many workers ran them
// and in whatever order they finished.
static void test_run_reports_jobs_in_job_order(void) {
  enum { JOB_COUNT = 12 };
  struct Arena arena = test_arena();
  struct Buf expected = {.arena = &arena};
  for (ptrdiff_t i = 0; i < JOB_COUNT; i++) {
    buf_printf(&expected, "job %td first\njob %td second\n", i, i);
  }
  for (ptrdiff_t worker_count = 1; worker_count <= TEST_WORKER_COUNT_MAX; worker_count++) {
    struct Log log = test_log();
    TEST_CHECK(run_jobs(JOB_COUNT, worker_count, fail_twice, NULL, &log) == -1);
    TEST_CHECK(test_log_is(&log, "%s", buf_cstr(&expected)));
    TEST_CHECK(log.errors == 2 * JOB_COUNT);
  }
}

// Each failed job's errors land on lines of their own, in job index order.
static void test_run_separates_errors_by_line(void) {
  struct FailureContext context = {.failure_count = 2};
  struct Log log = test_log();
  TEST_CHECK(run_jobs(3, 2, fail_selected_job, &context, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failure 0\nfailure 1\n"));
}

// Distinct failures past the cap are summarized as a count rather than listed, and still count as
// errors.
static void test_run_caps_collected_errors(void) {
  enum { EXTRA_FAILURE_COUNT = 5 };
  const ptrdiff_t failure_count = JOB_ERROR_REPORT_COUNT_MAX + EXTRA_FAILURE_COUNT;
  struct FailureContext context = {.failure_count = failure_count};
  struct Log log = test_log();
  TEST_CHECK(run_jobs(failure_count, 4, fail_selected_job, &context, &log) == -1);

  struct Arena arena = test_arena();
  struct Buf expected = {.arena = &arena};
  for (ptrdiff_t i = 0; i < JOB_ERROR_REPORT_COUNT_MAX; i++) {
    buf_printf(&expected, "failure %td\n", i);
  }
  buf_printf(&expected, "\xE2\x80\xA6 and %d more failures\n", EXTRA_FAILURE_COUNT);
  TEST_CHECK(test_log_is(&log, "%s", buf_cstr(&expected)));
  TEST_CHECK(log.errors == failure_count);
}

// A systematic failure repeats one message across every job. The report names that cause once and
// then the true failure count, rather than spending the cap on twenty identical lines.
static void test_run_reports_a_systematic_failure_once(void) {
  enum { JOB_COUNT = 200 };
  struct Log log = test_log();
  TEST_CHECK(run_jobs(JOB_COUNT, 4, fail_with_one_message, NULL, &log) == -1);

  // One detail plus one remainder line, not twenty copies plus a remainder. Every failure is still
  // counted, so the remainder is the other 199.
  TEST_CHECK(test_log_is(&log,
                         "template 'page.html' could not be parsed\n"
                         "\xE2\x80\xA6 and 199 more failures\n"));
  TEST_CHECK(log.errors == JOB_COUNT);
}

// A systematic failure that each job attributes to itself is still one cause. The report prints the
// first job's line whole, attribution included, and counts the rest.
static void test_run_reports_an_attributed_systematic_failure_once(void) {
  enum { JOB_COUNT = 50 };
  struct FailureContext context = {.failure_count = 1};
  struct Log log = test_log();
  TEST_CHECK(run_jobs(JOB_COUNT, 4, fail_with_attributed_cause, &context, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "cause 0 (while rendering 'content/0.md')\n"
                         "\xE2\x80\xA6 and 49 more failures\n"));
  TEST_CHECK(log.errors == JOB_COUNT);
}

// Attributed failures with different causes are each printed once, in job order, for the first job
// that had each cause.
static void test_run_reports_each_attributed_cause_once(void) {
  enum { JOB_COUNT = 9 };
  struct FailureContext context = {.failure_count = 3};
  struct Log log = test_log();
  TEST_CHECK(run_jobs(JOB_COUNT, 3, fail_with_attributed_cause, &context, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "cause 0 (while rendering 'content/0.md')\n"
                         "cause 1 (while rendering 'content/1.md')\n"
                         "cause 2 (while rendering 'content/2.md')\n"
                         "\xE2\x80\xA6 and 6 more failures\n"));
  TEST_CHECK(log.errors == JOB_COUNT);
}

// A phase whose jobs fail without recording an error still reports one, naming the phase.
static void test_run_names_a_silent_failure(void) {
  struct Log log = test_log();
  TEST_CHECK(run_jobs(2, 1, fail_silently, NULL, &log) == -1);
  TEST_CHECK(test_log_is(&log, "rendering failed without a diagnostic\n"));
  TEST_CHECK(log.errors == 1);
}

#ifdef __GLIBC__
// A phase that can start no worker thread names the phase and the system's reason, so a thread
// limit reads differently from a job that failed silently.
static void test_run_names_the_thread_creation_error(void) {
  pthread_attr_t saved;
  TEST_ASSERT(pthread_getattr_default_np(&saved) == 0);
  const int error_number = thread_create_fail_begin();
  if (TEST_CHECK(error_number != 0)) {
    struct Log log = test_log();
    TEST_CHECK(run_jobs(2, 2, succeed_job, NULL, &log) == -1);
    char message[ERROR_SYSTEM_MESSAGE_SIZE];
    TEST_CHECK(test_log_is(&log, "rendering: cannot start worker thread: %s\n",
                           error_system_message(message, sizeof(message), error_number)));
    TEST_CHECK(log.errors == 1);
  }
  (void)pthread_setattr_default_np(&saved);
  (void)pthread_attr_destroy(&saved);
}
#endif

TEST_LIST = {
    {"run accepts empty and successful sets", test_run_accepts_empty_and_successful_sets},
    {"run hands each job its worker arenas", test_run_hands_each_job_its_worker_arenas},
    {"run prints progress lines whole and in order",
     test_run_prints_progress_lines_whole_and_in_order},
    {"run keeps every error of a job", test_run_keeps_every_error_of_a_job},
    {"run reports jobs in job order", test_run_reports_jobs_in_job_order},
    {"run separates errors by line", test_run_separates_errors_by_line},
    {"run caps collected errors", test_run_caps_collected_errors},
    {"run reports a systematic failure once", test_run_reports_a_systematic_failure_once},
    {"run reports an attributed systematic failure once",
     test_run_reports_an_attributed_systematic_failure_once},
    {"run reports each attributed cause once", test_run_reports_each_attributed_cause_once},
    {"run names a silent failure", test_run_names_a_silent_failure},
#ifdef __GLIBC__
    {"run names the thread creation error", test_run_names_the_thread_creation_error},
#endif
    {NULL, NULL},
};
