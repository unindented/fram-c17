#include "runtime/memory.h"

#include <stddef.h>

#include "runtime/os.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** What one thread works on, for the out-of-memory report. */
struct MemoryOomJob {
  /** File the thread works on, or empty. */
  struct Str file_path;

  /** Log of the thread's current job, or `NULL`. */
  const struct Log* log;
};

/**
 * The command's log, which the out-of-memory report prints first. Only the main thread writes it,
 * and only while no worker runs, so every worker that reads it sees the value set before it
 * started.
 */
static const struct Log* memory_oom_log;

/** What the calling thread works on. Each thread has its own. */
static _Thread_local struct MemoryOomJob memory_oom_job;

void memory_oom_set_log(const struct Log* log) {
  memory_oom_log = log;
}

void memory_oom_set_job(struct Str file_path, const struct Log* log) {
  memory_oom_job = (struct MemoryOomJob){file_path, log};
}

void memory_oom_report(struct Os* os) {
  // Each piece is printed as it is, because building one message would need memory. A print that
  // fails cannot be reported either, so the results are discarded.
  if (memory_oom_log != NULL) {
    (void)os_print(os, 2, buf_str(&memory_oom_log->buf));
  }
  if (memory_oom_job.log != NULL) {
    (void)os_print(os, 2, buf_str(&memory_oom_job.log->buf));
  }
  (void)os_print(os, 2, S("fram: out of memory"));
  if (memory_oom_job.file_path.len > 0) {
    (void)os_print(os, 2, S(" (in '"));
    (void)os_print(os, 2, memory_oom_job.file_path);
    (void)os_print(os, 2, S("')"));
  }
  (void)os_print(os, 2, S("\n"));
}
