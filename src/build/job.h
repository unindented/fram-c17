#ifndef FRAM_JOB_H
#define FRAM_JOB_H

#include <stdbool.h>
#include <stddef.h>

struct JobSet;
struct StringBuffer;

/** Most distinct diagnostics one parallel phase reports before summarizing the rest as a count. */
enum { JOB_ERROR_REPORT_COUNT_MAX = 20 };

/**
 * @brief Callback run once per job index by `job_run`.
 *
 * @param jobs     Running job set, whose error slot for `index` the job may fill through
 *                 `job_set_error`. Must not be `NULL`.
 * @param index    Zero-based job index to process.
 * @param userdata Opaque pointer passed through from `job_run`. Every worker receives the same
 *                 pointer, so any mutation through it is the caller's to make safe.
 * @return `0` on success, or `-1` to mark the phase as failed.
 */
typedef int (*JobFn)(struct JobSet* jobs, size_t index, void* userdata);

/**
 * @brief Runs one parallel phase, reports progress, and collects its diagnostics.
 *
 * Allocates one error slot per job, runs every index through the worker pool, and appends each
 * distinct diagnostic to `error_out` as its own line, at most `JOB_ERROR_REPORT_COUNT_MAX` of them,
 * followed by a count of the remaining failures. A newline is written only between lines, so the
 * collection never starts with a blank line.
 *
 * @param job_count    Number of job indexes to run. `0` runs nothing and succeeds.
 * @param worker_count Requested worker threads, as for `pool_run`.
 * @param job_fn       Callback invoked for each index, concurrently from several threads, so it
 *                     must be thread-safe. Must not be `NULL`.
 * @param userdata     Opaque pointer forwarded to every `job_fn` call.
 * @param phase_label  Phase name leading each progress line, and named when the run fails without a
 *                     diagnostic. Must not be `NULL`.
 * @param is_verbose   Whether each finished job prints a progress line to `stderr`.
 * @param error_out    Growable buffer that receives the collected diagnostics. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a job failed, the error slots could not be
 *         allocated, the worker pool could not start, or a diagnostic could not be appended.
 */
int job_run(size_t job_count,
            size_t worker_count,
            JobFn job_fn,
            void* userdata,
            const char* phase_label,
            bool is_verbose,
            struct StringBuffer* error_out) __attribute__((nonnull(3, 5, 7)));

/**
 * @brief Records a job's first diagnostic in its error slot.
 *
 * Safe to call from worker threads because it writes only the job's own error slot. This is
 * first-wins: a later call for the same index is ignored, because the first message names the cause
 * and a later one is generally a consequence of it.
 *
 * @param jobs  Running job set passed to the job. Must not be `NULL`.
 * @param index Index of the calling job. A job writes only its own slot.
 * @param fmt   `printf` format for the diagnostic. Must not be `NULL`.
 * @param ...   Arguments for `fmt`.
 */
void job_set_error(struct JobSet* jobs, size_t index, const char* fmt, ...)
    __attribute__((format(printf, 3, 4), nonnull(1, 3)));

#endif
