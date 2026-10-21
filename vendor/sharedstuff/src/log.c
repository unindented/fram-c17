#include "shared/log.h"

#include <stdarg.h>
#include <stddef.h>

#include "shared/buf.h"
#include "shared/str.h"

/**
 * @brief Ends the error whose message was just appended to `log->buf`: appends the attribution to
 *        `file` and the newline, and counts the error.
 *
 * @param log  Log to append to. Must not be `NULL`.
 * @param file File the error was found in, or empty to name none.
 * @return `-1` always.
 */
static int log_error_finish(struct Log* log, struct Str file) __attribute__((nonnull(1)));

int log_error(struct Log* log, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int rc = log_error_v(log, format, args);
  va_end(args);
  return rc;
}

int log_error_v(struct Log* log, const char* format, va_list args) {
  buf_vprintf(&log->buf, format, args);
  return log_error_finish(log, (struct Str){0});
}

int log_error_in(struct Log* log, struct Str file, const char* format, ...) {
  va_list args;
  va_start(args, format);
  buf_vprintf(&log->buf, format, args);
  va_end(args);
  return log_error_finish(log, file);
}

void log_append(struct Log* log, const struct Log* source) {
  buf_append(&log->buf, buf_str(&source->buf));
  log->errors += source->errors;
}

static int log_error_finish(struct Log* log, struct Str file) {
  if (file.len > 0) {
    buf_printf(&log->buf, " (in '%.*s')", STR_FMT(file));
  }
  buf_append_char(&log->buf, '\n');
  log->errors++;
  return -1;
}
