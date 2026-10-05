// `flockfile`/`funlockfile` are POSIX rather than C17, so a strict `-std=c17` compile hides them
// unless the platform's feature-test macro asks for them: `_DARWIN_C_SOURCE` on macOS,
// `_DEFAULT_SOURCE` on glibc. Both must precede every `#include`, because a libc header resolves
// its own visibility guards the first time it is included.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "build/job.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "runtime/pool.h"
#include "shared/string_buffer.h"

/** One running parallel phase: its error slots and the adapter state around the caller's job. */
struct JobSet {
  /** One error message per job, indexed by job index. Empty means success. */
  char (*errors)[ERROR_MESSAGE_SIZE];

  /** Number of jobs, and therefore error slots. Printed as the denominator of the progress line. */
  size_t count;

  /** Caller's job, invoked once per index. Read-only during the run. */
  JobFn job_fn;

  /** Opaque context handed to every `job_fn` call, whose sharing is the caller's to make safe. */
  void* userdata;

  /** Phase name leading each progress line, and named when the run fails without a diagnostic. */
  const char* phase_label;

  /** Whether each finished job prints a progress line to `stderr`. */
  bool is_verbose;

  /**
   * Jobs finished so far. Every worker increments it, so it is atomic. The only other memory a
   * worker writes is its own error slot.
   */
  atomic_size_t completed_count;
};

/**
 * @brief Runs one job and prints its progress line. This is the `PoolJobFn` behind `job_run`.
 *
 * @param index    Job index to run.
 * @param userdata `struct JobSet*` of the running phase. Must not be `NULL`.
 * @return The job's own result: `0` on success, or `-1` on failure.
 */
static int run_one_job(size_t index, void* userdata) __attribute__((nonnull(2)));

/**
 * @brief Appends every distinct job diagnostic, the remainder count, and a fallback diagnostic.
 *
 * Runs after `pool_run` has joined every worker, so the slots are read unsynchronized.
 *
 * @param jobs      Finished job set. Must not be `NULL`.
 * @param pool_rc   Result of `pool_run`.
 * @param error_out Growable buffer receiving the diagnostics. Must not be `NULL`.
 * @return `0` when the phase succeeded, or `-1` when it failed or a diagnostic could not be
 *         appended.
 */
static int run_report_errors(const struct JobSet* jobs, int pool_rc, struct StringBuffer* error_out)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Reports whether a message was already appended to this run's report.
 *
 * @param reported       Borrowed messages appended so far. Must not be `NULL`.
 * @param reported_count Number of entries in `reported`.
 * @param message        Candidate message. Must not be `NULL`.
 * @return `true` when an identical message was already reported.
 */
static bool is_reported_message(const char* const* reported,
                                size_t reported_count,
                                const char* message) __attribute__((nonnull(1, 3)));

/**
 * @brief Appends one diagnostic to a phase's collected error text as its own line.
 *
 * A newline is written only when `buffer` already holds text, so the collection never starts with a
 * blank line.
 *
 * @param buffer  Growable collection of phase diagnostics. Must not be `NULL`.
 * @param message Terminated message to append. Must not be `NULL`.
 * @return `0` when the message was appended, or `-1` on allocation failure, leaving `buffer`
 *         unchanged.
 */
static int append_error(struct StringBuffer* buffer, const char* message)
    __attribute__((nonnull(1, 2)));

int job_run(size_t job_count,
            size_t worker_count,
            JobFn job_fn,
            void* userdata,
            const char* phase_label,
            bool is_verbose,
            struct StringBuffer* error_out) {
  struct JobSet jobs = {
      .count = job_count,
      .job_fn = job_fn,
      .userdata = userdata,
      .phase_label = phase_label,
      .is_verbose = is_verbose,
  };
  atomic_init(&jobs.completed_count, 0);
  jobs.errors = calloc(job_count, sizeof(*jobs.errors));
  if (jobs.errors == NULL && job_count > 0) {
    (void)append_error(error_out, "out of memory allocating job error slots");
    return -1;
  }

  const int pool_rc = pool_run(job_count, worker_count, run_one_job, &jobs);

  // `pool_run` joins every worker before returning, and that join is the happens-before edge that
  // publishes each job's writes to its own error slot. Everything below therefore runs with no
  // worker alive. The slot reads need no atomics. The `stderr` write needs no `flockfile`, and a
  // version of this that read the slots before the join would be a data race.

  // Close the progress line before any later status lines.
  if (is_verbose && job_count > 0) {
    (void)fputc('\n', stderr);
  }
  const int rc = run_report_errors(&jobs, pool_rc, error_out);
  free(jobs.errors);
  return rc;
}

void job_set_error(struct JobSet* jobs, size_t index, const char* fmt, ...) {
  if (index >= jobs->count || jobs->errors[index][0] != '\0') {
    return;
  }

  // This is first-wins, like the other error latches (`record_error` and `render_fail`). The first
  // message names the cause. A later one is generally a consequence and must not replace it.
  va_list ap;
  va_start(ap, fmt);
  // This goes through `error_report_va` rather than `vsnprintf` directly, so it marks an over-long
  // message as truncated. These messages compose a callee's whole diagnostic with a source path
  // into one `ERROR_MESSAGE_SIZE` buffer, so they are the ones most likely to be cut. An unmarked
  // cut leaves a path that reads as complete and names no existing file.
  error_report_va(jobs->errors[index], sizeof(jobs->errors[index]), fmt, ap);
  va_end(ap);
}

static int run_one_job(size_t index, void* userdata) {
  struct JobSet* jobs = userdata;
  const int rc = jobs->job_fn(jobs, index, jobs->userdata);
  const size_t completed =
      atomic_fetch_add_explicit(&jobs->completed_count, 1, memory_order_relaxed) + 1;
  if (jobs->is_verbose) {
    // Progress is best-effort. A full or closed `stderr` must not fail a phase whose jobs
    // succeeded, so both write results are discarded. The flush is required because a progress line
    // ends in `\r` rather than a newline and `stderr` may be line-buffered. It stays inside the
    // lock, so no other worker's line can land between the write and its flush.
    flockfile(stderr);
    (void)fprintf(stderr, "\r%s %zu/%zu", jobs->phase_label, completed, jobs->count);
    (void)fflush(stderr);
    funlockfile(stderr);
  }
  return rc;
}

static int run_report_errors(const struct JobSet* jobs,
                             int pool_rc,
                             struct StringBuffer* error_out) {
  // Report each distinct message once rather than the first twenty slots. One systematic failure --
  // a template that cannot parse, an unreadable shared input -- fails every job with the same text,
  // and capping by slot spends the whole budget on twenty copies of one line. Deduplicating means
  // the report names every different cause it saw, and the remainder below still carries the true
  // scale.
  const char* reported[JOB_ERROR_REPORT_COUNT_MAX] = {NULL};
  size_t failure_count = 0;
  size_t reported_count = 0;
  for (size_t i = 0; i < jobs->count; i++) {
    const char* message = jobs->errors[i];
    if (message[0] == '\0') {
      continue;
    }
    failure_count++;
    if (is_reported_message(reported, reported_count, message)) {
      continue;
    }
    if (reported_count == JOB_ERROR_REPORT_COUNT_MAX) {
      continue;
    }
    if (append_error(error_out, message) != 0) {
      return -1;
    }
    // The slots are stable after the join, so borrowing each message is enough to compare against.
    reported[reported_count] = message;
    reported_count++;
  }

  if (failure_count > reported_count) {
    char summary[ERROR_MESSAGE_SIZE];
    error_report(summary, sizeof(summary), "\xE2\x80\xA6 and %zu more failures",
                 failure_count - reported_count);
    if (append_error(error_out, summary) != 0) {
      return -1;
    }
  }

  if (pool_rc != 0) {
    // `pool_run` can fail before any job records a per-slot diagnostic (e.g. no worker thread could
    // be started), so append a generic message rather than failing silently at the command
    // boundary.
    if (failure_count == 0) {
      char message[ERROR_MESSAGE_SIZE];
      error_report(message, sizeof(message), "%s failed without a diagnostic", jobs->phase_label);
      if (append_error(error_out, message) != 0) {
        return -1;
      }
    }
    return -1;
  }
  return 0;
}

static bool is_reported_message(const char* const* reported,
                                size_t reported_count,
                                const char* message) {
  for (size_t i = 0; i < reported_count; i++) {
    if (strcmp(reported[i], message) == 0) {
      return true;
    }
  }
  return false;
}

static int append_error(struct StringBuffer* buffer, const char* message) {
  // Reserve the separator and the message together so a failure leaves the buffer unchanged rather
  // than ending it with a dangling newline. Both appends below then fit the reserved capacity.
  const size_t separator_len = buffer->len > 0 ? 1 : 0;
  if (string_buffer_reserve(buffer, separator_len + strlen(message)) != 0) {
    return -1;
  }
  if (separator_len > 0 && string_buffer_append_char(buffer, '\n') != 0) {
    return -1;
  }
  return string_buffer_append(buffer, message);
}
