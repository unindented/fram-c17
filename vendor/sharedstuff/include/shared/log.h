#ifndef SHARED_LOG_H
#define SHARED_LOG_H

#include <stdarg.h>
#include <stddef.h>

#include "shared/buf.h"
#include "shared/str.h"

/**
 * Growable diagnostic log: every error a run finds, one line each, in the order they were found.
 *
 * Start one as `(struct Log){.buf = {.arena = arena}}`. The arena must be one nothing else
 * allocates from through a copy while the log is in use, because a function given a copy of an
 * arena by value would hand out the same bytes the log grows into. A region of its own is the
 * simple way to guarantee that. A message is never truncated, so it can name every value in full,
 * and a caller that keeps going after an error collects every error in one run and checks `errors`
 * before doing anything it cannot undo.
 *
 * Two logs never share an arena, so each thread can keep its own, and the owner concatenates them
 * in a fixed order once the threads are done.
 */
struct Log {
  /** Messages appended so far, each ending in `\n`. Print it as it stands. */
  struct Buf buf;

  /** Number of errors `log_error` recorded. */
  ptrdiff_t errors;
};

/**
 * @brief Records one error: appends the text that the format produces as one line, and counts it.
 *
 * Use it as `log_error(log, "failed to read '%.*s': %.*s", STR_FMT(path), STR_FMT(reason))`. The
 * compiler checks the arguments against the format, as for `buf_printf`. Returns `-1`, so an action
 * can write `return log_error(log, ...);`.
 *
 * @param log    Log to append to. Must not be `NULL`.
 * @param format `printf` format of the message, without the newline. Must not be `NULL`.
 * @param ...    Arguments that the format converts.
 * @return `-1` always.
 */
int log_error(struct Log* log, const char* format, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Records one error from a `va_list`, as `log_error` does.
 *
 * This is the form for a function that forwards its own format and arguments. That function must
 * carry `__attribute__((format(printf, n, m)))` itself, so the compiler checks its callers.
 *
 * @param log    Log to append to. Must not be `NULL`.
 * @param format `printf` format of the message, without the newline. Must not be `NULL`.
 * @param args   Arguments that the format converts. This call consumes them.
 * @return `-1` always.
 */
int log_error_v(struct Log* log, const char* format, va_list args)
    __attribute__((format(printf, 2, 0), nonnull(1, 2)));

/**
 * @brief Records one error found inside `file`: the message, then ` (in '<file>')`, as one line.
 *
 * This is the attribution for a failure located inside a file, such as a parse error, as in
 * `frontmatter key 'draft' must be a boolean (in 'content/post.md')`. An empty `file` adds nothing,
 * so a caller with no file to name passes `(struct Str){0}`.
 *
 * @param log    Log to append to. Must not be `NULL`.
 * @param file   File the error was found in, or empty to name none.
 * @param format `printf` format of the message, without the attribution. Must not be `NULL`.
 * @param ...    Arguments that the format converts.
 * @return `-1` always.
 */
int log_error_in(struct Log* log, struct Str file, const char* format, ...)
    __attribute__((format(printf, 3, 4), nonnull(1, 3)));

/**
 * @brief Appends every message of `source` to `log`, and adds its error count.
 *
 * @param log    Log to append to. Must not be `NULL`.
 * @param source Log whose messages are copied. Left unchanged. Must not be `NULL`.
 */
void log_append(struct Log* log, const struct Log* source) __attribute__((nonnull(1, 2)));

#endif
