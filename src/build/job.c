#include "build/job.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/error.h"
#include "runtime/os.h"
#include "runtime/pool.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** One running parallel phase: its jobs' logs and the adapter state around the caller's job. */
struct JobSet {
  /**
   * One log per job, indexed by job index. A job with no errors succeeded.
   */
  struct Log* logs;

  /** Number of jobs, and therefore logs. Printed as the denominator of the progress line. */
  ptrdiff_t count;

  /** One set of arenas per worker, indexed by the worker index `pool_run` passes each job. */
  struct JobArenas* worker_arenas;

  /**
   * One arena per worker, indexed like `worker_arenas`, holding the logs of that worker's jobs. A
   * worker runs one job at a time, so its jobs' logs never grow at once.
   */
  struct Arena* worker_log_arenas;

  /** Caller's job, invoked once per index. Read-only during the run. */
  JobFn job_fn;

  /** Opaque context handed to every `job_fn` call, whose sharing is the caller's to make safe. */
  void* userdata;

  /** Phase name leading each progress line, and named when the run fails without a diagnostic. */
  struct Str phase_label;

  /** Text that starts a job's own name at the end of a line, or empty for none. */
  struct Str attribution;

  /** Whether each finished job prints a progress line to `stderr`. */
  bool is_verbose;

  /** Host the progress lines are printed on. */
  struct Os* os;

  /**
   * Jobs finished so far. Every worker increments it, so it is atomic. The only other memory a
   * worker writes is its own jobs' logs, its own entry of `worker_log_arenas` and its own arenas.
   */
  atomic_ptrdiff_t completed_count;
};

/**
 * @brief Divides the free space of `arena` into one log share per worker and one more, and gives
 *        each worker one of them. A step of `job_run`.
 *
 * @param arena        Arena the shares are taken from, which keeps the last one. Must not be
 *                     `NULL`.
 * @param worker_count Requested worker threads. Must be positive.
 * @return One log arena per worker.
 */
static struct Arena* job_run_split_worker_logs(struct Arena* arena, ptrdiff_t worker_count)
    __attribute__((nonnull(1)));

/**
 * @brief Runs one job and prints its progress line. This is the `PoolJobFn` behind `job_run`.
 *
 * @param index    Job index to run.
 * @param worker   Index of the worker running the job, which selects its arenas.
 * @param userdata `struct JobSet*` of the running phase. Must not be `NULL`.
 * @return The job's own result: `0` on success, or `-1` on failure.
 */
static int job_run_one(ptrdiff_t index, ptrdiff_t worker, void* userdata)
    __attribute__((nonnull(3)));

/**
 * @brief Counts a finished job and prints the progress line that names the new count. A step of
 *        `job_run_one`.
 *
 * @param jobs   Running phase, whose count it raises and whose host, label and job count the line
 *               uses. Must not be `NULL`.
 * @param worker Index of the worker that ran the job, whose scratch region holds the line.
 */
static void job_run_one_print_progress(struct JobSet* jobs, ptrdiff_t worker)
    __attribute__((nonnull(1)));

/**
 * @brief Appends every failed job's log in job order, the remainder count, and a fallback error.
 *
 * Runs after `pool_run` has joined every worker, so the logs are read unsynchronized.
 *
 * @param jobs    Finished job set. Must not be `NULL`.
 * @param pool_rc Result of `pool_run`.
 * @param log     Log receiving the errors. Must not be `NULL`.
 * @return `0` when the phase succeeded, or `-1` when it failed.
 */
static int job_run_report_errors(const struct JobSet* jobs, int pool_rc, struct Log* log)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Reports whether a job's messages repeat the messages of an earlier job.
 *
 * @param reported       Messages of earlier jobs, appended to the report. Must not be `NULL`.
 * @param reported_count Number of entries in `reported`.
 * @param messages       Candidate job's messages.
 * @param attribution    Text that starts a job's own name at the end of a line, or empty for none.
 * @return `true` when an earlier job's messages are the same, line for line, up to each line's
 *         attribution.
 */
static bool is_reported_message(const struct Str* reported,
                                ptrdiff_t reported_count,
                                struct Str messages,
                                struct Str attribution) __attribute__((nonnull(1)));

/**
 * @brief Reports whether two jobs' messages are the same, line for line, up to each line's
 *        attribution. A step of `is_reported_message`.
 *
 * @param reported    Messages of an earlier job, each line ending in `\n`.
 * @param messages    Candidate job's messages, each line ending in `\n`.
 * @param attribution Text that starts a job's own name at the end of a line, or empty for none.
 * @return `true` when both have the same number of lines and each pair agrees.
 */
static bool is_reported_message_same(struct Str reported,
                                     struct Str messages,
                                     struct Str attribution);

/**
 * @brief Returns `line` up to the last `attribution` in it, or all of it when there is none.
 *
 * @param line        One line, without its `\n`.
 * @param attribution Text to look for. Empty finds nothing.
 * @return The part of `line` before its attribution.
 */
static struct Str line_cause(struct Str line, struct Str attribution);

int job_run(ptrdiff_t job_count,
            ptrdiff_t worker_count,
            struct JobArenas* worker_arenas,
            JobFn job_fn,
            void* userdata,
            struct Str phase_label,
            struct Str attribution,
            bool is_verbose,
            struct Os* os,
            struct Arena scratch,
            struct Log* log) {
  assert(job_count >= 0);
  assert(worker_count > 0);
  struct JobSet jobs = {
      .count = job_count,
      .worker_arenas = worker_arenas,
      .job_fn = job_fn,
      .userdata = userdata,
      .phase_label = phase_label,
      .attribution = attribution,
      .is_verbose = is_verbose,
      .os = os,
  };
  atomic_init(&jobs.completed_count, 0);
  jobs.logs = arena_new(&scratch, job_count, struct Log);

  jobs.worker_log_arenas = job_run_split_worker_logs(&scratch, worker_count);
  const int pool_rc = pool_run(job_count, worker_count, job_run_one, &jobs, scratch);
  // `pool_run` leaves `pthread_create`'s error in `errno` when no worker started, so it is read
  // before a print below can change it.
  const int pool_error = errno;

  // `pool_run` joins every worker before returning, and that join is the happens-before edge that
  // publishes each job's writes to its own log. Everything below therefore runs with no worker
  // alive. The log reads need no atomics. The print needs no lock, and a version of this that read
  // the logs before the join would be a data race.

  // Close the progress line before any later status lines.
  if (is_verbose && job_count > 0) {
    (void)os_print(os, 2, S("\n"));
  }
  // A started worker runs every job, so no finished job means no worker started, a failure that no
  // job's log can explain.
  if (pool_rc != 0 && atomic_load(&jobs.completed_count) == 0) {
    char message[ERROR_SYSTEM_MESSAGE_SIZE];
    return log_error(log, "%.*s: cannot start worker thread: %s", STR_FMT(phase_label),
                     error_system_message(message, sizeof(message), pool_error));
  }
  return job_run_report_errors(&jobs, pool_rc, log);
}

static struct Arena* job_run_split_worker_logs(struct Arena* arena, ptrdiff_t worker_count) {
  // Each worker's logs get an equal share of the region, carved off before the pool's own
  // bookkeeping, so no two workers and no worker and the pool grow into the same bytes.
  struct Arena* worker_logs = arena_new(arena, worker_count, struct Arena);
  const char* arena_end = arena->limit != NULL ? arena->limit : arena->end;
  const ptrdiff_t share_len = (arena_end - arena->beg) / (worker_count + 1);
  for (ptrdiff_t i = 0; i < worker_count; i++) {
    worker_logs[i] = arena_split(arena, share_len);
  }
  return worker_logs;
}

static int job_run_one(ptrdiff_t index, ptrdiff_t worker, void* userdata) {
  struct JobSet* jobs = userdata;
  struct Log* log = &jobs->logs[index];
  *log = (struct Log){.buf = {.arena = &jobs->worker_log_arenas[worker]}};
  const int rc = jobs->job_fn(index, &jobs->worker_arenas[worker], log, jobs->userdata);
  if (jobs->is_verbose) {
    job_run_one_print_progress(jobs, worker);
  } else {
    (void)atomic_fetch_add_explicit(&jobs->completed_count, 1, memory_order_relaxed);
  }
  return rc;
}

static void job_run_one_print_progress(struct JobSet* jobs, ptrdiff_t worker) {
  // Workers finish jobs at once, so the line is formatted whole and printed in one call under a
  // lock of its own. `os_print` retries a short write, and the lock keeps another worker's line
  // from landing between two parts of one. The count is taken under the same lock, so the lines
  // print in count order and the last one names every job. The line goes in the worker's scratch
  // region, which the job that just ended left as it found it.
  static pthread_mutex_t progress_lock = PTHREAD_MUTEX_INITIALIZER;
  struct Arena scratch = jobs->worker_arenas[worker].scratch;
  struct Buf line = {.arena = &scratch};
  (void)pthread_mutex_lock(&progress_lock);
  const ptrdiff_t completed =
      atomic_fetch_add_explicit(&jobs->completed_count, 1, memory_order_relaxed) + 1;
  buf_printf(&line, "\r%.*s %td/%td", STR_FMT(jobs->phase_label), completed, jobs->count);
  // Progress is best-effort. A full or closed `stderr` must not fail a phase whose jobs succeeded,
  // so the print's result is discarded.
  (void)os_print(jobs->os, 2, buf_str(&line));
  (void)pthread_mutex_unlock(&progress_lock);
}

static int job_run_report_errors(const struct JobSet* jobs, int pool_rc, struct Log* log) {
  // Report each distinct job once rather than the first twenty that failed. One systematic failure
  // -- a template that cannot parse, an unreadable shared input -- fails every job with the same
  // text, and capping by job spends the whole budget on twenty copies of one line. Deduplicating
  // means the report names every different cause it saw, and the remainder below still carries the
  // true scale. A job's messages are compared as a whole, because a job that failed several ways
  // says so in one block. Each line compares only up to the job's own name, so a cause that every
  // job attributes to itself still collapses into one.
  struct Str reported[JOB_ERROR_REPORT_COUNT_MAX] = {0};
  ptrdiff_t failure_count = 0;
  ptrdiff_t reported_count = 0;
  for (ptrdiff_t i = 0; i < jobs->count; i++) {
    const struct Log* job_log = &jobs->logs[i];
    if (job_log->errors == 0) {
      continue;
    }
    failure_count++;
    const struct Str messages = buf_str(&job_log->buf);
    if (reported_count == JOB_ERROR_REPORT_COUNT_MAX ||
        is_reported_message(reported, reported_count, messages, jobs->attribution)) {
      // The error still counts, so the caller's check of `errors` sees every failed job.
      log->errors += job_log->errors;
      continue;
    }
    log_append(log, job_log);
    // The logs are stable after the join, so a view of each is enough to compare against.
    reported[reported_count] = messages;
    reported_count++;
  }

  if (failure_count > reported_count) {
    // This summarizes errors already counted, so it is not one itself.
    buf_printf(&log->buf, "\xE2\x80\xA6 and %td more failures\n", failure_count - reported_count);
  }

  if (pool_rc != 0 || failure_count > 0) {
    // A job can fail without recording an error, so record a generic one rather than failing
    // silently at the command boundary.
    if (failure_count == 0) {
      (void)log_error(log, "%.*s failed without a diagnostic", STR_FMT(jobs->phase_label));
    }
    return -1;
  }
  return 0;
}

static bool is_reported_message(const struct Str* reported,
                                ptrdiff_t reported_count,
                                struct Str messages,
                                struct Str attribution) {
  for (ptrdiff_t i = 0; i < reported_count; i++) {
    if (is_reported_message_same(reported[i], messages, attribution)) {
      return true;
    }
  }
  return false;
}

static bool is_reported_message_same(struct Str reported,
                                     struct Str messages,
                                     struct Str attribution) {
  while (reported.len > 0 && messages.len > 0) {
    const struct StrCut reported_line = str_cut(reported, '\n');
    const struct StrCut messages_line = str_cut(messages, '\n');
    if (!str_eq(line_cause(reported_line.head, attribution),
                line_cause(messages_line.head, attribution))) {
      return false;
    }
    reported = reported_line.tail;
    messages = messages_line.tail;
  }
  return reported.len == 0 && messages.len == 0;
}

static struct Str line_cause(struct Str line, struct Str attribution) {
  if (attribution.len == 0) {
    return line;
  }
  for (ptrdiff_t i = line.len - attribution.len; i >= 0; i--) {
    if (str_startswith(str_tail(line, i), attribution)) {
      return str_head(line, i);
    }
  }
  return line;
}
