#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "build/job.h"
#include "shared/string_buffer.h"

/** Shared state for the failing jobs below: how many of them fail. */
struct FailureContext {
  /** Number of leading jobs that fail. */
  size_t failure_count;
};

/**
 * @brief Counts the lines in a collected diagnostic buffer.
 *
 * @param buffer Collected diagnostics, one per line with no trailing newline. Must not be `NULL`.
 * @return `0` for an empty buffer, otherwise one more than the number of newlines.
 */
static size_t count_lines(const struct StringBuffer* buffer) {
  size_t line_count = buffer->len > 0 ? 1 : 0;
  for (size_t i = 0; i < buffer->len; i++) {
    if (buffer->data[i] == '\n') {
      line_count++;
    }
  }
  return line_count;
}

/**
 * @brief Fails the first `failure_count` jobs, each with a message naming its own index.
 *
 * Distinct messages are what exercise the report cap itself rather than its deduplication.
 *
 * @param jobs     Running job set receiving the diagnostic.
 * @param index    Job index this call owns.
 * @param userdata `struct FailureContext` holding the failure count.
 * @return `0` when the index is past the failing range, or `-1` after recording a diagnostic.
 */
static int fail_selected_job(struct JobSet* jobs, size_t index, void* userdata) {
  const struct FailureContext* context = userdata;
  if (index >= context->failure_count) {
    return 0;
  }
  job_set_error(jobs, index, "failure %zu", index);
  return -1;
}

/**
 * @brief Fails every job with one identical message.
 *
 * This is the shape a systematic failure takes: one cause repeated across every job, which is what
 * deduplication has to collapse.
 *
 * @param jobs     Running job set receiving the diagnostic.
 * @param index    Job index this call owns.
 * @param userdata Unused.
 * @return `-1` always, after recording the shared diagnostic.
 */
static int fail_with_one_message(struct JobSet* jobs, size_t index, void* userdata) {
  (void)userdata;
  job_set_error(jobs, index, "template 'page.html' could not be parsed");
  return -1;
}

/**
 * @brief Records two diagnostics for its own index, of which only the first may be kept.
 *
 * @param jobs     Running job set receiving the diagnostics.
 * @param index    Job index this call owns.
 * @param userdata Unused.
 * @return `-1` always, after recording both diagnostics.
 */
static int fail_twice(struct JobSet* jobs, size_t index, void* userdata) {
  (void)userdata;
  job_set_error(jobs, index, "first");
  job_set_error(jobs, index, "second");
  return -1;
}

/**
 * @brief Succeeds without recording anything.
 *
 * @param jobs     Unused job set.
 * @param index    Unused job index.
 * @param userdata Unused.
 * @return `0` always.
 */
static int succeed_job(struct JobSet* jobs, size_t index, void* userdata) {
  (void)jobs;
  (void)index;
  (void)userdata;
  return 0;
}

// A job's first diagnostic is kept and a later one for the same index is dropped.
static void test_set_error_keeps_the_first_diagnostic(void) {
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(job_run(1, 1, fail_twice, NULL, "rendering", false, &error_buffer) == -1);
  TEST_ASSERT(error_buffer.data != NULL);
  TEST_CHECK(strcmp(error_buffer.data, "first") == 0);
  TEST_MSG("actual: '%s'", error_buffer.data);
  string_buffer_free(&error_buffer);
}

// The first collected diagnostic starts the report without a leading newline, and each later one
// lands on its own line, in job index order.
static void test_run_separates_diagnostics_by_line(void) {
  struct FailureContext context = {.failure_count = 2};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(job_run(3, 2, fail_selected_job, &context, "rendering", false, &error_buffer) == -1);
  TEST_ASSERT(error_buffer.data != NULL);
  TEST_CHECK(strcmp(error_buffer.data, "failure 0\nfailure 1") == 0);
  TEST_CHECK(error_buffer.len == strlen("failure 0\nfailure 1"));
  string_buffer_free(&error_buffer);
}

// Distinct diagnostics past the cap are summarized as a count rather than listed.
static void test_run_caps_collected_errors(void) {
  enum { EXTRA_FAILURE_COUNT = 5 };
  const size_t failure_count = JOB_ERROR_REPORT_COUNT_MAX + EXTRA_FAILURE_COUNT;
  struct FailureContext context = {.failure_count = failure_count};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(job_run(failure_count, 4, fail_selected_job, &context, "rendering", false,
                     &error_buffer) == -1);

  TEST_ASSERT(error_buffer.data != NULL);
  TEST_CHECK(count_lines(&error_buffer) == JOB_ERROR_REPORT_COUNT_MAX + 1);
  TEST_CHECK(strstr(error_buffer.data, "failure 0\n") != NULL);
  TEST_CHECK(strstr(error_buffer.data, "failure 20") == NULL);
  TEST_CHECK(strstr(error_buffer.data, "\n\xE2\x80\xA6 and 5 more failures") != NULL);

  string_buffer_free(&error_buffer);
}

// A systematic failure repeats one message across every job. The report names that cause once and
// then the true failure count, rather than spending the cap on twenty identical lines.
static void test_run_reports_a_systematic_failure_once(void) {
  enum { JOB_COUNT = 200 };
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(
      job_run(JOB_COUNT, 4, fail_with_one_message, NULL, "rendering", false, &error_buffer) == -1);

  // One detail plus one remainder line, not twenty copies plus a remainder. Every failure is still
  // counted, so the remainder is the other 199.
  TEST_ASSERT(error_buffer.data != NULL);
  TEST_CHECK(strcmp(error_buffer.data,
                    "template 'page.html' could not be parsed\n"
                    "\xE2\x80\xA6 and 199 more failures") == 0);

  string_buffer_free(&error_buffer);
}

// An empty phase and a phase whose jobs all succeed both succeed without a diagnostic.
static void test_run_accepts_empty_and_successful_sets(void) {
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(job_run(0, 2, succeed_job, NULL, "rendering", false, &error_buffer) == 0);
  TEST_CHECK(job_run(3, 2, succeed_job, NULL, "rendering", false, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  string_buffer_free(&error_buffer);
}

TEST_LIST = {
    {"set error keeps the first diagnostic", test_set_error_keeps_the_first_diagnostic},
    {"run separates diagnostics by line", test_run_separates_diagnostics_by_line},
    {"run caps collected errors", test_run_caps_collected_errors},
    {"run reports a systematic failure once", test_run_reports_a_systematic_failure_once},
    {"run accepts empty and successful sets", test_run_accepts_empty_and_successful_sets},
    {NULL, NULL},
};
