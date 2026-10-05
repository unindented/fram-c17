#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "runtime/proc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/error.h"
#include "core/text.h"

extern char** environ;

/**
 * Size of the standard-error tail retained from one child process, in bytes including the `NUL`
 * terminator.
 *
 * The tail only feeds a failure reason, and every reason ends up inside an `ERROR_MESSAGE_SIZE`
 * diagnostic, so retaining more than that could never be shown.
 */
enum { STDERR_TAIL_SIZE = ERROR_MESSAGE_SIZE };

/**
 * @brief Size in bytes of a system-message scratch buffer, including the `NUL` terminator.
 *
 * Separate from `STDERR_TAIL_SIZE`, which bounds the retained `stderr` tail. These buffers hold one
 * `strerror` string.
 */
enum { PROC_MESSAGE_SIZE = 256 };

/** Marks the front of a standard-error tail that was cut to fit a failure reason. */
static const char* const TAIL_CUT_MARKER = "...";

/** One process stream being drained without blocking the other. */
struct ProcStream {
  /** Read end of the child's pipe, set to -1 once it reached EOF and was closed. */
  int fd;

  /**
   * Whether this is the child's standard error, whose bytes go to the bounded tail kept for a
   * failure diagnostic rather than to the caller's output buffer.
   */
  bool is_stderr;
};

/** Copies the argument vector because `posix_spawnp` does not accept const strings. */
static char** copy_argv(const char* const* argv, char* reason, size_t reason_len)
    __attribute__((nonnull(1)));

/** Frees an argument vector returned by `copy_argv`. */
static void free_argv(char** argv);

/** Sets one descriptor to nonblocking mode. */
static int set_nonblocking(int fd, char* reason, size_t reason_len);

/** Retains the newest bytes in a bounded standard-error tail. */
static void append_tail(char* tail, size_t* tail_len, const char* data, size_t data_len)
    __attribute__((nonnull(1, 2, 3)));

/** Drains one ready pipe until it would block or reaches EOF. */
static int drain_stream(struct ProcStream* stream,
                        char* output,
                        size_t output_len,
                        size_t* output_used,
                        bool* is_output_truncated,
                        char* stderr_tail,
                        size_t* stderr_tail_len,
                        char* reason,
                        size_t reason_len) __attribute__((nonnull(1, 4, 5, 6, 7)));

/** Waits for one child, retrying an interrupted wait. */
static int wait_for_child(pid_t pid, int* status_out, char* reason, size_t reason_len)
    __attribute__((nonnull(2)));

/**
 * @brief Reports a failed child status, ending the reason with the newest standard error that fits.
 *
 * @param status          Child status from `waitpid`.
 * @param stderr_tail     Newest standard-error bytes, with room for a terminator after
 *                        `stderr_tail_len`. Modified in place. Must not be `NULL`.
 * @param stderr_tail_len Number of bytes at `stderr_tail`, below `STDERR_TAIL_SIZE`.
 * @param reason          Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len      Size of `reason` in bytes.
 * @return `-1` always.
 */
static int report_child_failure(int status,
                                char* stderr_tail,
                                size_t stderr_tail_len,
                                char* reason,
                                size_t reason_len) __attribute__((nonnull(2)));

int proc_run(const char* const* argv,
             char* output,
             size_t output_len,
             char* reason,
             size_t reason_len) {
  if (argv[0] == NULL || argv[0][0] == '\0') {
    return error_report(reason, reason_len, "program name is empty");
  }
  if (output_len > 0) {
    output[0] = '\0';
  }

  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
    const int saved_errno = errno;
    if (stdout_pipe[0] >= 0) {
      (void)close(stdout_pipe[0]);
      (void)close(stdout_pipe[1]);
    }
    if (stderr_pipe[0] >= 0) {
      (void)close(stderr_pipe[0]);
      (void)close(stderr_pipe[1]);
    }
    char message[PROC_MESSAGE_SIZE];
    return error_report(reason, reason_len, "cannot create process pipe: %s",
                        error_system_message(message, sizeof(message), saved_errno));
  }

  posix_spawn_file_actions_t actions;
  int action_rc = posix_spawn_file_actions_init(&actions);
  const bool is_actions_initialized = action_rc == 0;
  if (action_rc == 0) {
    action_rc = posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
  }
  if (action_rc == 0) {
    action_rc = posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
  }
  const int descriptors[] = {stdout_pipe[0], stdout_pipe[1], stderr_pipe[0], stderr_pipe[1]};
  for (size_t i = 0; action_rc == 0 && i < sizeof(descriptors) / sizeof(descriptors[0]); i++) {
    if (descriptors[i] != STDOUT_FILENO && descriptors[i] != STDERR_FILENO) {
      action_rc = posix_spawn_file_actions_addclose(&actions, descriptors[i]);
    }
  }

  char** mutable_argv = action_rc == 0 ? copy_argv(argv, reason, reason_len) : NULL;
  pid_t pid = -1;
  int spawn_rc = action_rc;
  if (action_rc == 0 && mutable_argv != NULL) {
    // `posix_spawn` is the thread-safe process boundary. Replacing this with `fork` would run an
    // async-signal-unsafe child path while gallery workers are active.
    spawn_rc = posix_spawnp(&pid, mutable_argv[0], &actions, NULL, mutable_argv, environ);
  }
  if (is_actions_initialized) {
    (void)posix_spawn_file_actions_destroy(&actions);
  }
  free_argv(mutable_argv);
  (void)close(stdout_pipe[1]);
  (void)close(stderr_pipe[1]);

  if (spawn_rc != 0 || mutable_argv == NULL) {
    (void)close(stdout_pipe[0]);
    (void)close(stderr_pipe[0]);
    if (mutable_argv == NULL && action_rc == 0) {
      return -1;
    }
    if (spawn_rc == ENOENT) {
      return error_report(reason, reason_len, "program not found ('%s')", argv[0]);
    }
    char message[PROC_MESSAGE_SIZE];
    return error_report(reason, reason_len, "cannot start program: %s ('%s')",
                        error_system_message(message, sizeof(message), spawn_rc), argv[0]);
  }

  int rc = 0;
  if (set_nonblocking(stdout_pipe[0], reason, reason_len) != 0 ||
      set_nonblocking(stderr_pipe[0], reason, reason_len) != 0) {
    rc = -1;
  }

  struct ProcStream streams[] = {
      {.fd = stdout_pipe[0], .is_stderr = false},
      {.fd = stderr_pipe[0], .is_stderr = true},
  };
  size_t output_used = 0;
  bool is_output_truncated = false;
  char stderr_tail[STDERR_TAIL_SIZE];
  size_t stderr_tail_len = 0;

  while (rc == 0 && (streams[0].fd >= 0 || streams[1].fd >= 0)) {
    struct pollfd poll_fds[2];
    nfds_t poll_count = 0;
    for (size_t i = 0; i < 2; i++) {
      if (streams[i].fd >= 0) {
        poll_fds[poll_count++] = (struct pollfd){.fd = streams[i].fd, .events = POLLIN | POLLHUP};
      }
    }
    int poll_rc;
    do {
      poll_rc = poll(poll_fds, poll_count, -1);
    } while (poll_rc < 0 && errno == EINTR);
    if (poll_rc < 0) {
      char message[PROC_MESSAGE_SIZE];
      rc = error_report(reason, reason_len, "cannot poll process output: %s",
                        error_system_message(message, sizeof(message), errno));
      break;
    }
    for (nfds_t i = 0; rc == 0 && i < poll_count; i++) {
      if ((poll_fds[i].revents & POLLNVAL) != 0) {
        rc = error_report(reason, reason_len, "process pipe became invalid");
        break;
      }
      if ((poll_fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }
      struct ProcStream* stream = poll_fds[i].fd == streams[0].fd ? &streams[0] : &streams[1];
      rc = drain_stream(stream, output, output_len, &output_used, &is_output_truncated, stderr_tail,
                        &stderr_tail_len, reason, reason_len);
    }
  }

  for (size_t i = 0; i < 2; i++) {
    if (streams[i].fd >= 0) {
      (void)close(streams[i].fd);
    }
  }
  if (output_len > 0) {
    output[output_used] = '\0';
  }

  if (rc != 0) {
    (void)kill(pid, SIGTERM);
  }
  int status = 0;
  if (wait_for_child(pid, &status, reason, reason_len) != 0) {
    return -1;
  }
  if (rc != 0) {
    return -1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return report_child_failure(status, stderr_tail, stderr_tail_len, reason, reason_len);
  }
  if (is_output_truncated) {
    return error_report(reason, reason_len, "process output exceeds %zu-byte buffer",
                        output_len - 1);
  }
  return 0;
}

static char** copy_argv(const char* const* argv, char* reason, size_t reason_len) {
  size_t count = 0;
  while (argv[count] != NULL) {
    if (count == SIZE_MAX / sizeof(char*) - 1) {
      (void)error_report(reason, reason_len, "too many process arguments");
      return NULL;
    }
    count++;
  }
  char** copy = calloc(count + 1, sizeof(*copy));
  if (copy == NULL) {
    (void)error_report(reason, reason_len, "out of memory");
    return NULL;
  }
  for (size_t i = 0; i < count; i++) {
    copy[i] = text_strdup(argv[i]);
    if (copy[i] == NULL) {
      free_argv(copy);
      (void)error_report(reason, reason_len, "out of memory");
      return NULL;
    }
  }
  return copy;
}

static void free_argv(char** argv) {
  if (argv == NULL) {
    return;
  }
  for (size_t i = 0; argv[i] != NULL; i++) {
    free(argv[i]);
  }
  free(argv);
}

static int set_nonblocking(int fd, char* reason, size_t reason_len) {
  const int flags = fcntl(fd, F_GETFL);
  if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
    return 0;
  }
  char message[PROC_MESSAGE_SIZE];
  return error_report(reason, reason_len, "cannot configure process pipe: %s",
                      error_system_message(message, sizeof(message), errno));
}

static void append_tail(char* tail, size_t* tail_len, const char* data, size_t data_len) {
  const size_t capacity = STDERR_TAIL_SIZE - 1;
  if (data_len >= capacity) {
    memcpy(tail, data + data_len - capacity, capacity);
    *tail_len = capacity;
    return;
  }
  if (*tail_len > capacity - data_len) {
    const size_t discard = *tail_len - (capacity - data_len);
    memmove(tail, tail + discard, *tail_len - discard);
    *tail_len -= discard;
  }
  memcpy(tail + *tail_len, data, data_len);
  *tail_len += data_len;
}

static int drain_stream(struct ProcStream* stream,
                        char* output,
                        size_t output_len,
                        size_t* output_used,
                        bool* is_output_truncated,
                        char* stderr_tail,
                        size_t* stderr_tail_len,
                        char* reason,
                        size_t reason_len) {
  char buffer[4096];
  for (;;) {
    const ssize_t nread = read(stream->fd, buffer, sizeof(buffer));
    if (nread > 0) {
      const size_t count = (size_t)nread;
      if (stream->is_stderr) {
        append_tail(stderr_tail, stderr_tail_len, buffer, count);
      } else if (output_len > 0) {
        const size_t capacity = output_len - 1;
        const size_t available = capacity - *output_used;
        const size_t copied = count < available ? count : available;
        memcpy(output + *output_used, buffer, copied);
        *output_used += copied;
        *is_output_truncated = *is_output_truncated || copied != count;
      }
      continue;
    }
    if (nread == 0) {
      (void)close(stream->fd);
      stream->fd = -1;
      return 0;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    char message[PROC_MESSAGE_SIZE];
    return error_report(reason, reason_len, "cannot read process output: %s",
                        error_system_message(message, sizeof(message), errno));
  }
}

static int wait_for_child(pid_t pid, int* status_out, char* reason, size_t reason_len) {
  pid_t waited;
  do {
    waited = waitpid(pid, status_out, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited == pid) {
    return 0;
  }
  char message[PROC_MESSAGE_SIZE];
  return error_report(reason, reason_len, "cannot wait for process: %s",
                      error_system_message(message, sizeof(message), errno));
}

static int report_child_failure(int status,
                                char* stderr_tail,
                                size_t stderr_tail_len,
                                char* reason,
                                size_t reason_len) {
  while (stderr_tail_len > 0 &&
         (stderr_tail[stderr_tail_len - 1] == '\n' || stderr_tail[stderr_tail_len - 1] == '\r')) {
    stderr_tail_len--;
  }
  stderr_tail[stderr_tail_len] = '\0';
  if (WIFEXITED(status)) {
    if (stderr_tail_len > 0) {
      // `error_report` keeps the head of a message that does not fit, but the head of a tail is its
      // oldest text and the child's final line usually names the cause. The tail is therefore cut
      // from the front to the room left after the prefix, and marked where the cut was made.
      const int prefix_len =
          snprintf(NULL, 0, "process exited with status %d: ", WEXITSTATUS(status));
      const size_t marker_len = strlen(TAIL_CUT_MARKER);
      const char* shown = stderr_tail;
      if (prefix_len > 0 && reason_len > (size_t)prefix_len + marker_len + 1) {
        const size_t room = reason_len - (size_t)prefix_len - 1;
        if (stderr_tail_len > room) {
          char* cut = stderr_tail + stderr_tail_len - room;
          memcpy(cut, TAIL_CUT_MARKER, marker_len);
          shown = cut;
        }
      }
      return error_report(reason, reason_len, "process exited with status %d: %s",
                          WEXITSTATUS(status), shown);
    }
    return error_report(reason, reason_len, "process exited with status %d", WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    return error_report(reason, reason_len, "process terminated by signal %d", WTERMSIG(status));
  }
  return error_report(reason, reason_len, "process ended without an exit status");
}
