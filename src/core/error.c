#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "core/error.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Guard against a buffer too small to hold a path plus surrounding context.
_Static_assert(ERROR_MESSAGE_SIZE >= 64, "error message buffer too small for useful diagnostics");

/**
 * This is written over the tail of a message that did not fit, so a cut value cannot be read as a
 * whole one. It is three periods rather than a word, because it costs the fewest bytes of the
 * message it is marking, and because a reader already reads three periods as a cut.
 *
 * This is the one diagnostic allowed to end in punctuation. Diagnostics otherwise carry none, so
 * they compose as fragments. A marker that only ever appears where the fragment was already severed
 * does not compose with anything.
 */
static const char TRUNCATION_MARKER[] = "...";

int error_report(char* err, size_t err_len, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  error_report_va(err, err_len, fmt, ap);
  va_end(ap);
  return -1;
}

void error_report_va(char* err, size_t err_len, const char* fmt, va_list ap) {
  // This truncates a diagnostic too long for the buffer rather than turning it into a second
  // failure. Every composed message is reason-first, so truncation costs the path and never the
  // cause. This also permits `err` to be `NULL` when `err_len` is 0, which the call sites that want
  // no message pass.
  const int n = vsnprintf(err, err_len, fmt, ap);

  // This reads the return value for whether the message was cut, not whether the call failed.
  // Several messages truncate by construction rather than as an edge case. A limit-exceeded message
  // naming an over-long value can never fit the value it is about. Without a marker, a truncated
  // path looks complete and can name a file that does not exist. `vsnprintf` returns the length it
  // *would* have written, so a value at or past `err_len` means truncation. A negative return is an
  // encoding error, unreachable here, because this project has no wide or multibyte conversions.
  //
  // The third test keeps the write in bounds. Below `sizeof(TRUNCATION_MARKER)` there is nowhere to
  // put the marker, so an over-long message is cut without one rather than writing past the buffer
  // to add a marker. Without the test, `err_len - sizeof(TRUNCATION_MARKER)` wraps and the
  // arithmetic runs on `err` even when it is `NULL`.
  if (n > 0 && (size_t)n >= err_len && err_len >= sizeof(TRUNCATION_MARKER)) {
    memcpy(err + err_len - sizeof(TRUNCATION_MARKER), TRUNCATION_MARKER, sizeof(TRUNCATION_MARKER));
  }
}

const char* error_system_message(char* message, size_t message_len, int error_number) {
  // This is the thread-safe variant. `strerror` shares one buffer across threads, and render jobs
  // report filesystem failures concurrently.
  if (strerror_r(error_number, message, message_len) != 0) {
    (void)snprintf(message, message_len, "system error %d", error_number);
  }
  return message;
}
