#ifndef FRAM_ERROR_H
#define FRAM_ERROR_H

#include <stdarg.h>
#include <stddef.h>

/**
 * Size in bytes of a diagnostic message buffer, including the `NUL` terminator.
 *
 * This is knowingly smaller than `OUTPUT_PATH_RELATIVE_LEN_MAX` (1024). Sizing every `err` buffer
 * in the program for a worst-case path costs more than that case is worth, so a pathological path
 * may truncate. Every composed message leads with the failed operation and cause so they cannot
 * truncate. Any unbounded value comes last. Do not raise this size to make a path fit, and do not
 * reverse that ordering to make the message read better.
 */
enum { ERROR_MESSAGE_SIZE = 512 };

/**
 * @brief Formats a diagnostic into `err` and returns a failure code for call-site chaining.
 *
 * Writes at most `err_len` bytes including the `NUL` terminator. It truncates a message that does
 * not fit and replaces its tail with `...`, so a cut value cannot be read as a complete one. Always
 * returns `-1` so a caller can write `return error_report(...);`.
 *
 * @param err     Destination buffer for the message. May be `NULL` only when `err_len` is 0.
 * @param err_len Size of `err` in bytes.
 * @param fmt     `printf`-style format string. Must not be `NULL`.
 * @param ...     Arguments for `fmt`.
 * @return `-1` always.
 */
int error_report(char* err, size_t err_len, const char* fmt, ...)
    __attribute__((format(printf, 3, 4), nonnull(3)));

/**
 * @brief Formats a diagnostic into `err` from a `va_list`, the shared core for reporters.
 *
 * Writes at most `err_len` bytes including the `NUL` terminator, marking a message that did not fit
 * as described for `error_report`. Consumes `ap`. The caller owns its `va_start`/`va_end` pairing
 * and must `va_copy` before the call if it needs to reuse the argument list.
 *
 * @param err     Destination buffer for the message. May be `NULL` only when `err_len` is 0.
 * @param err_len Size of `err` in bytes.
 * @param fmt     `printf`-style format string. Must not be `NULL`.
 * @param ap      Arguments for `fmt`.
 */
void error_report_va(char* err, size_t err_len, const char* fmt, va_list ap)
    __attribute__((format(printf, 3, 0), nonnull(3)));

/**
 * @brief Writes the system message for `error_number` into `message`.
 *
 * This is the only place that turns an `errno` into text. Callers compose the result into their own
 * message. It is thread safe, so a render job may call it.
 *
 * @param message      Destination buffer. Must hold at least one byte. Must not be `NULL`.
 * @param message_len  Size of `message` in bytes. Must be non-zero.
 * @param error_number `errno` value to describe.
 * @return `message`, holding a terminated message.
 */
const char* error_system_message(char* message, size_t message_len, int error_number)
    __attribute__((nonnull(1)));

#endif
