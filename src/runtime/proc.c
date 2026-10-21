// `pipe2` is a GNU extension in glibc and musl, so Linux needs `_GNU_SOURCE` to see it. macOS has
// no `pipe2` and needs `_DARWIN_C_SOURCE` for `POSIX_SPAWN_CLOEXEC_DEFAULT`. Both must precede
// every `#include`, because a libc header resolves its own visibility guards the first time it is
// included.
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE

#include "runtime/proc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/error.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

extern char** environ;

/**
 * Bytes of a child's standard error that `proc_run` keeps for a failure message. `ffmpeg` can write
 * without limit, and the line that names the cause is usually its last, so the tail keeps the
 * newest bytes, a few lines' worth, and drops the older ones.
 */
enum { STDERR_TAIL_SIZE = 4096 };

/**
 * Size of the one-line form of a standard-error tail. Each byte of the tail becomes one byte, and a
 * line break at most two.
 */
enum { TAIL_LINE_SIZE = 2 * STDERR_TAIL_SIZE };

/** Marks the front of a standard-error tail whose older bytes were dropped. */
static const struct Str TAIL_CUT_MARKER = STR_INIT("...");

/**
 * Where `proc_run` reports its failure: the caller's operation, which leads the message, the file
 * the child works on, which trails it when there is one, and the log.
 */
struct ProcReport {
  /** Operation the caller was performing, such as `failed to probe media`. */
  struct Str operation;

  /** ` ('<file>')` naming the file the child works on, which ends every message, or empty. */
  struct Str file_suffix;

  /** Log that receives the message. */
  struct Log* log;
};

/** The newest bytes of a child's standard error. */
struct ProcTail {
  /** Bytes kept, oldest first. */
  char data[STDERR_TAIL_SIZE];

  /** Number of bytes in `data`. */
  ptrdiff_t len;

  /** Whether older bytes were dropped to keep the newest ones. */
  bool is_cut;
};

/** One process stream being drained without blocking the other. */
struct ProcStream {
  /** Read end of the child's pipe, set to -1 once it reached EOF and was closed. */
  int fd;

  /**
   * Whether this is the child's standard error, whose bytes go to the bounded tail kept for a
   * failure message rather than to the captured output.
   */
  bool is_stderr;
};

/**
 * @brief Copies the argument vector because `posix_spawnp` does not accept const strings.
 *
 * @param argv  Null-terminated argument vector to copy. Must not be `NULL`.
 * @param arena Arena that owns the copy. Must not be `NULL`.
 * @return Null-terminated copy of `argv` owned by `arena`.
 */
static char** copy_argv(const char* const* argv, struct Arena* arena)
    __attribute__((nonnull(1, 2), returns_nonnull));

/**
 * @brief Creates a pipe whose two ends no child inherits.
 *
 * Linux has `pipe2`, which sets close-on-exec atomically. macOS has none, but `proc_run` spawns
 * every child with `POSIX_SPAWN_CLOEXEC_DEFAULT` there, which keeps only the descriptors its file
 * actions name. Elsewhere `fcntl` sets the flag after `pipe`, which leaves a window in which
 * another thread's child can inherit the ends.
 *
 * @param fds Receives the read and the write end. Must not be `NULL`.
 * @return `0` on success, or `-1` with `errno` set and no descriptor left open.
 */
static int open_pipe(int fds[static 2]);

/**
 * @brief Spawns `argv` with `/dev/null` as standard input and the two pipe ends as its standard
 *        output and standard error.
 *
 * @param argv      Null-terminated argument vector whose first entry is the program. Must not be
 *                  `NULL`.
 * @param stdout_fd Write end the child gets as its standard output.
 * @param stderr_fd Write end the child gets as its standard error.
 * @param pid_out   Receives the child's process ID on success. Must not be `NULL`.
 * @return `0` on success, or the error number of the step that failed.
 */
static int spawn_child(char** argv, int stdout_fd, int stderr_fd, pid_t* pid_out)
    __attribute__((nonnull(1, 4)));

/**
 * @brief Reports a child that could not be started, naming the program.
 *
 * @param spawn_rc Error number `spawn_child` returned.
 * @param program  Terminated name of the program. Must not be `NULL`.
 * @param report   Where the failure is reported. Must not be `NULL`.
 * @return `-1` always.
 */
static int report_spawn_failure(int spawn_rc, const char* program, const struct ProcReport* report)
    __attribute__((nonnull(2, 3)));

/**
 * @brief Drains the child's two pipes until both reach EOF or a step fails, then closes them. A
 *        step of `proc_run`.
 *
 * Both read ends are set to nonblocking mode first, so draining one cannot block while the child
 * waits to write to the other.
 *
 * @param streams      The child's standard output and standard error, in that order. Each
 *                     descriptor is closed when this returns. Must not be `NULL`.
 * @param output       Buffer that receives standard output, up to `PROC_OUTPUT_LEN_MAX` bytes, or
 *                     `NULL` to discard it.
 * @param output_total Running count of the standard-output bytes the child wrote, kept or not. Must
 *                     not be `NULL`.
 * @param tail         Tail that receives standard error. Must not be `NULL`.
 * @param report       Where a failure is reported. Must not be `NULL`.
 * @return `0` once both pipes reached EOF, or `-1` on a failure.
 */
static int proc_run_drain(struct ProcStream streams[static 2],
                          struct Buf* output,
                          ptrdiff_t* output_total,
                          struct ProcTail* tail,
                          const struct ProcReport* report) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Sets one descriptor to nonblocking mode.
 *
 * @param fd     Descriptor to configure.
 * @param report Where a failure is reported. Must not be `NULL`.
 * @return `0` on success, or `-1` on failure.
 */
static int set_nonblocking(int fd, const struct ProcReport* report) __attribute__((nonnull(2)));

/**
 * @brief Keeps the newest bytes of a child's standard error, dropping the oldest when it is full.
 *
 * @param tail Tail to append to. Must not be `NULL`.
 * @param data Bytes the child wrote.
 */
static void append_tail(struct ProcTail* tail, struct Str data) __attribute__((nonnull(1)));

/**
 * @brief Drains one ready pipe until it would block or reaches EOF.
 *
 * @param stream       Stream to drain. Its descriptor is closed and set to -1 at EOF. Must not be
 *                     `NULL`.
 * @param output       Buffer that receives standard output, up to `PROC_OUTPUT_LEN_MAX` bytes, or
 *                     `NULL` to discard it.
 * @param output_total Running count of the standard-output bytes the child wrote, kept or not. Must
 *                     not be `NULL`.
 * @param tail         Tail that receives standard error. Must not be `NULL`.
 * @param report       Where a read failure is reported. Must not be `NULL`.
 * @return `0` when the pipe would block or reached EOF, or `-1` on a read failure.
 */
static int drain_stream(struct ProcStream* stream,
                        struct Buf* output,
                        ptrdiff_t* output_total,
                        struct ProcTail* tail,
                        const struct ProcReport* report) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Waits for one child, retrying an interrupted wait.
 *
 * @param pid        Child to wait for.
 * @param status_out Receives the child's status. Must not be `NULL`.
 * @param report     Where a failure is reported. Must not be `NULL`.
 * @return `0` once the child is reaped, or `-1` on a wait failure.
 */
static int wait_for_child(pid_t pid, int* status_out, const struct ProcReport* report)
    __attribute__((nonnull(2, 3)));

/**
 * @brief Reports a failed child status, ending the message with the standard-error tail.
 *
 * @param status Child status from `waitpid`.
 * @param tail   Newest standard-error bytes. Must not be `NULL`.
 * @param report Where the failure is reported. Must not be `NULL`.
 * @return `-1` always.
 */
static int report_child_failure(int status,
                                const struct ProcTail* tail,
                                const struct ProcReport* report) __attribute__((nonnull(2, 3)));

/**
 * @brief Puts a standard-error tail on one line, as `log.h` asks of every error. A step of
 *        `report_child_failure`.
 *
 * Trailing line breaks are dropped, each run of line breaks inside becomes `; `, and a `NUL` byte,
 * which would end the text early, becomes `?`.
 *
 * @param tail      Newest standard-error bytes. Must not be `NULL`.
 * @param line_data Buffer that receives the line.
 * @return The line, in `line_data`, or an empty text when the tail holds only line breaks.
 */
static struct Str report_child_failure_line(const struct ProcTail* tail,
                                            char line_data[static TAIL_LINE_SIZE])
    __attribute__((nonnull(1)));

/**
 * @brief Reports `cause` as the reason the caller's operation failed, naming the file when there is
 *        one.
 *
 * @param report Where the failure is reported. Must not be `NULL`.
 * @param cause  Lowercase cause, such as `process pipe became invalid`.
 * @return `-1` always, so a caller can write `return report_cause(...);`.
 */
static int report_cause(const struct ProcReport* report, struct Str cause)
    __attribute__((nonnull(1)));

/**
 * @brief Reports a failed step of running the child, with the system message for `error_number`.
 *
 * @param report       Where the failure is reported. Must not be `NULL`.
 * @param step         Lowercase step that failed, such as `create process pipe`.
 * @param error_number `errno` value to describe.
 * @return `-1` always, so a caller can write `return report_errno(...);`.
 */
static int report_errno(const struct ProcReport* report, struct Str step, int error_number)
    __attribute__((nonnull(1)));

int proc_run(const char* const* argv,
             struct Arena* arena,
             struct Str* output_out,
             struct Str operation,
             struct Str file_path,
             struct Log* log) {
  // The file is formatted once, so each message below is one format that ends with it.
  struct Buf file_suffix = {.arena = arena};
  if (file_path.len > 0) {
    buf_printf(&file_suffix, " ('%.*s')", STR_FMT(file_path));
  }
  const struct ProcReport report = {operation, buf_str(&file_suffix), log};
  if (argv[0] == NULL || argv[0][0] == '\0') {
    return report_cause(&report, S("program name is empty"));
  }

  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (open_pipe(stdout_pipe) != 0) {
    return report_errno(&report, S("create process pipe"), errno);
  }
  if (open_pipe(stderr_pipe) != 0) {
    const int pipe_errno = errno;
    (void)close(stdout_pipe[0]);
    (void)close(stdout_pipe[1]);
    return report_errno(&report, S("create process pipe"), pipe_errno);
  }

  pid_t pid = -1;
  const int spawn_rc = spawn_child(copy_argv(argv, arena), stdout_pipe[1], stderr_pipe[1], &pid);
  (void)close(stdout_pipe[1]);
  (void)close(stderr_pipe[1]);
  if (spawn_rc != 0) {
    (void)close(stdout_pipe[0]);
    (void)close(stderr_pipe[0]);
    return report_spawn_failure(spawn_rc, argv[0], &report);
  }

  struct ProcStream streams[] = {
      {.fd = stdout_pipe[0], .is_stderr = false},
      {.fd = stderr_pipe[0], .is_stderr = true},
  };
  // The output grows in `arena` past the copy of `argv`, where nothing else allocates while the
  // child runs, so it extends in place.
  struct Buf output = {.arena = arena};
  ptrdiff_t output_total = 0;
  struct ProcTail tail = {.len = 0};
  const int rc =
      proc_run_drain(streams, output_out != NULL ? &output : NULL, &output_total, &tail, &report);
  if (rc != 0) {
    (void)kill(pid, SIGTERM);
  }
  int status = 0;
  if (wait_for_child(pid, &status, &report) != 0) {
    return -1;
  }
  if (rc != 0) {
    return -1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return report_child_failure(status, &tail, &report);
  }
  if (output_out != NULL && output_total > PROC_OUTPUT_LEN_MAX) {
    return log_error(
        log, "%.*s: process output exceeds max output length (%d bytes) at %td bytes%.*s",
        STR_FMT(operation), PROC_OUTPUT_LEN_MAX, output_total, STR_FMT(report.file_suffix));
  }
  if (output_out != NULL) {
    *output_out = buf_str(&output);
  }
  return 0;
}

static char** copy_argv(const char* const* argv, struct Arena* arena) {
  ptrdiff_t count = 0;
  while (argv[count] != NULL) {
    count++;
  }
  // The arena zeroes the array, so the slot after the last argument is already its terminator.
  char** copy = arena_new(arena, count + 1, char*);
  for (ptrdiff_t i = 0; i < count; i++) {
    copy[i] = str_cstr(str_from_cstr(argv[i]), arena);
  }
  return copy;
}

static int open_pipe(int fds[static 2]) {
#if defined(__linux__)
  return pipe2(fds, O_CLOEXEC);
#else
  if (pipe(fds) != 0) {
    return -1;
  }
#if !defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
  if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
    const int fcntl_errno = errno;
    (void)close(fds[0]);
    (void)close(fds[1]);
    errno = fcntl_errno;
    return -1;
  }
#endif
  return 0;
#endif
}

static int spawn_child(char** argv, int stdout_fd, int stderr_fd, pid_t* pid_out) {
  // The child reads nothing, and `POSIX_SPAWN_CLOEXEC_DEFAULT` closes descriptor 0 too, so standard
  // input is opened explicitly on every platform. Without it, the child's first `open` could become
  // its standard input. `dup2` clears close-on-exec on its target, so the child keeps both pipes as
  // its standard output and standard error.
  posix_spawn_file_actions_t actions;
  int action_rc = posix_spawn_file_actions_init(&actions);
  const bool is_actions_initialized = action_rc == 0;
  if (action_rc == 0) {
    action_rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  }
  if (action_rc == 0) {
    action_rc = posix_spawn_file_actions_adddup2(&actions, stdout_fd, STDOUT_FILENO);
  }
  if (action_rc == 0) {
    action_rc = posix_spawn_file_actions_adddup2(&actions, stderr_fd, STDERR_FILENO);
  }
  posix_spawnattr_t attributes;
  int attribute_rc = posix_spawnattr_init(&attributes);
  const bool is_attributes_initialized = attribute_rc == 0;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
  if (attribute_rc == 0) {
    attribute_rc = posix_spawnattr_setflags(&attributes, (short)POSIX_SPAWN_CLOEXEC_DEFAULT);
  }
#endif

  int spawn_rc = action_rc != 0 ? action_rc : attribute_rc;
  if (spawn_rc == 0) {
    // `posix_spawn` is the thread-safe process boundary. Replacing this with `fork` would run an
    // async-signal-unsafe child path while gallery workers are active.
    spawn_rc = posix_spawnp(pid_out, argv[0], &actions, &attributes, argv, environ);
  }
  if (is_attributes_initialized) {
    (void)posix_spawnattr_destroy(&attributes);
  }
  if (is_actions_initialized) {
    (void)posix_spawn_file_actions_destroy(&actions);
  }
  return spawn_rc;
}

static int report_spawn_failure(int spawn_rc,
                                const char* program,
                                const struct ProcReport* report) {
  if (spawn_rc == ENOENT) {
    return log_error(report->log, "%.*s: program '%s' not found%.*s", STR_FMT(report->operation),
                     program, STR_FMT(report->file_suffix));
  }
  char message[ERROR_SYSTEM_MESSAGE_SIZE];
  (void)error_system_message(message, sizeof(message), spawn_rc);
  return log_error(report->log, "%.*s: cannot start program '%s': %s%.*s",
                   STR_FMT(report->operation), program, message, STR_FMT(report->file_suffix));
}

static int proc_run_drain(struct ProcStream streams[static 2],
                          struct Buf* output,
                          ptrdiff_t* output_total,
                          struct ProcTail* tail,
                          const struct ProcReport* report) {
  int rc = 0;
  if (set_nonblocking(streams[0].fd, report) != 0 || set_nonblocking(streams[1].fd, report) != 0) {
    rc = -1;
  }
  while (rc == 0 && (streams[0].fd >= 0 || streams[1].fd >= 0)) {
    struct pollfd poll_fds[2];
    nfds_t poll_count = 0;
    for (ptrdiff_t i = 0; i < 2; i++) {
      if (streams[i].fd >= 0) {
        poll_fds[poll_count++] = (struct pollfd){.fd = streams[i].fd, .events = POLLIN | POLLHUP};
      }
    }
    int poll_rc;
    do {
      poll_rc = poll(poll_fds, poll_count, -1);
    } while (poll_rc < 0 && errno == EINTR);
    if (poll_rc < 0) {
      rc = report_errno(report, S("poll process output"), errno);
      break;
    }
    for (nfds_t i = 0; rc == 0 && i < poll_count; i++) {
      if ((poll_fds[i].revents & POLLNVAL) != 0) {
        rc = report_cause(report, S("process pipe became invalid"));
        break;
      }
      if ((poll_fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }
      struct ProcStream* stream = poll_fds[i].fd == streams[0].fd ? &streams[0] : &streams[1];
      rc = drain_stream(stream, output, output_total, tail, report);
    }
  }

  for (ptrdiff_t i = 0; i < 2; i++) {
    if (streams[i].fd >= 0) {
      (void)close(streams[i].fd);
      streams[i].fd = -1;
    }
  }
  return rc;
}

static int set_nonblocking(int fd, const struct ProcReport* report) {
  const int flags = fcntl(fd, F_GETFL);
  if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
    return 0;
  }
  return report_errno(report, S("configure process pipe"), errno);
}

static void append_tail(struct ProcTail* tail, struct Str data) {
  const ptrdiff_t capacity = STDERR_TAIL_SIZE;
  if (data.len >= capacity) {
    tail->is_cut = tail->is_cut || tail->len > 0 || data.len > capacity;
    memcpy(tail->data, data.data + data.len - capacity, (size_t)capacity);
    tail->len = capacity;
    return;
  }
  if (tail->len > capacity - data.len) {
    const ptrdiff_t discard = tail->len - (capacity - data.len);
    memmove(tail->data, tail->data + discard, (size_t)(tail->len - discard));
    tail->len -= discard;
    tail->is_cut = true;
  }
  memcpy(tail->data + tail->len, data.data, (size_t)data.len);
  tail->len += data.len;
}

static int drain_stream(struct ProcStream* stream,
                        struct Buf* output,
                        ptrdiff_t* output_total,
                        struct ProcTail* tail,
                        const struct ProcReport* report) {
  char buffer[4096];
  for (;;) {
    const ssize_t read_len = read(stream->fd, buffer, sizeof(buffer));
    if (read_len > 0) {
      const struct Str data = {buffer, read_len};
      if (stream->is_stderr) {
        append_tail(tail, data);
      } else if (output != NULL) {
        // Bytes past the limit are counted but not kept, so the failure can say how much the child
        // wrote without holding all of it.
        const ptrdiff_t room =
            *output_total < PROC_OUTPUT_LEN_MAX ? PROC_OUTPUT_LEN_MAX - *output_total : 0;
        buf_append(output, str_head(data, data.len < room ? data.len : room));
        *output_total += data.len;
      }
      continue;
    }
    if (read_len == 0) {
      (void)close(stream->fd);
      stream->fd = -1;
      return 0;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN) {
      return 0;
    }
    return report_errno(report, S("read process output"), errno);
  }
}

static int wait_for_child(pid_t pid, int* status_out, const struct ProcReport* report) {
  pid_t waited;
  do {
    waited = waitpid(pid, status_out, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited == pid) {
    return 0;
  }
  return report_errno(report, S("wait for process"), errno);
}

static int report_child_failure(int status,
                                const struct ProcTail* tail,
                                const struct ProcReport* report) {
  char line_data[TAIL_LINE_SIZE];
  const struct Str line = report_child_failure_line(tail, line_data);
  const struct Str marker = tail->is_cut ? TAIL_CUT_MARKER : (struct Str){0};
  const struct Str operation = report->operation;
  const struct Str file_suffix = report->file_suffix;
  if (WIFEXITED(status)) {
    if (line.len == 0) {
      return log_error(report->log, "%.*s: process exited with status %d%.*s", STR_FMT(operation),
                       WEXITSTATUS(status), STR_FMT(file_suffix));
    }
    // The child's final line usually names the cause, so the tail keeps the newest bytes, and the
    // marker shows where older ones were dropped.
    return log_error(report->log, "%.*s: process exited with status %d: %.*s%.*s%.*s",
                     STR_FMT(operation), WEXITSTATUS(status), STR_FMT(marker), STR_FMT(line),
                     STR_FMT(file_suffix));
  }
  if (WIFSIGNALED(status)) {
    return log_error(report->log, "%.*s: process terminated by signal %d%.*s", STR_FMT(operation),
                     WTERMSIG(status), STR_FMT(file_suffix));
  }
  return report_cause(report, S("process ended without an exit status"));
}

static struct Str report_child_failure_line(const struct ProcTail* tail,
                                            char line_data[static TAIL_LINE_SIZE]) {
  struct Str shown = {tail->data, tail->len};
  while (shown.len > 0 &&
         (shown.data[shown.len - 1] == '\n' || shown.data[shown.len - 1] == '\r')) {
    shown = str_head(shown, shown.len - 1);
  }
  struct Str line = {line_data, 0};
  bool is_after_break = false;
  for (ptrdiff_t i = 0; i < shown.len; i++) {
    const char byte = shown.data[i];
    if (byte == '\n' || byte == '\r') {
      is_after_break = true;
      continue;
    }
    if (is_after_break && line.len > 0) {
      line_data[line.len++] = ';';
      line_data[line.len++] = ' ';
    }
    is_after_break = false;
    line_data[line.len++] = byte == '\0' ? '?' : byte;
  }
  return line;
}

static int report_cause(const struct ProcReport* report, struct Str cause) {
  return log_error(report->log, "%.*s: %.*s%.*s", STR_FMT(report->operation), STR_FMT(cause),
                   STR_FMT(report->file_suffix));
}

static int report_errno(const struct ProcReport* report, struct Str step, int error_number) {
  char message[ERROR_SYSTEM_MESSAGE_SIZE];
  (void)error_system_message(message, sizeof(message), error_number);
  return log_error(report->log, "%.*s: cannot %.*s: %s%.*s", STR_FMT(report->operation),
                   STR_FMT(step), message, STR_FMT(report->file_suffix));
}
