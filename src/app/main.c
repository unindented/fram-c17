#include "app/cli_dispatch.h"

#include <pthread.h>
#include <stdlib.h>

#include "app/exit_code.h"
#include "runtime/fs.h"
#include "runtime/memory.h"
#include "shared/arena.h"

/**
 * @brief Reports that the program ran out of memory and ends the process with `EXIT_CODE_FAILURE`.
 *
 * This is the executable's member of the `runtime/os.h` interface that `shared/arena.h` declares.
 * It is defined here rather than in `runtime/fs.c`, because what running out of memory does is a
 * decision about the program, not about the host: the unit tests define their own, whichever host
 * they link.
 *
 * It runs when an arena cannot commit the pages an allocation needs, when the system refuses a
 * reservation the program cannot work without, and, where a vendored library allocates with
 * `malloc`, when the wrapper around that library sees `malloc` fail. None of these has a recovery
 * the program could attempt, so the policy is to stop: this is the one place that decides it, and
 * no allocation site checks for failure. `memory_oom_report` prints the log the running command
 * named and names the input in progress, so the user still learns what failed. The reasons the
 * program can stop there are in the "Out of memory exits the process" decision in ARCHITECTURE.md.
 *
 * In a program that runs worker threads it may run on one, so it uses `_Exit`, which runs no
 * `atexit` handlers that another thread could be using. Nothing is buffered to lose, because every
 * print goes through `os_print`. Two workers can run out at once, so the lock lets the first print
 * its report and exit while the second waits for that exit. A program with one thread takes the
 * lock uncontended.
 */
_Noreturn void os_oom(void) {
  static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
  (void)pthread_mutex_lock(&report_lock);
  memory_oom_report(os_posix());
  _Exit(EXIT_CODE_FAILURE);
}

/**
 * @brief Process entry point. Delegates to `cli_dispatch` on the POSIX host.
 *
 * It only calls `cli_dispatch`, so every command decision stays in a file the unit tests can link.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return The exit code `cli_dispatch` resolved.
 */
int main(int argc, char** argv) {
  // The cast is required, not cosmetic. The underlying type of `enum ExitCode` is implementation
  // defined and may be unsigned, so returning one uncast trips `-Wsign-conversion` under `-Werror`.
  // That looks like an inconsistency to tidy away, and tidying it breaks the build.
  return (int)cli_dispatch(argc, argv, os_posix());
}
