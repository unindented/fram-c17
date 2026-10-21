#ifndef FRAM_ERROR_H
#define FRAM_ERROR_H

#include <stddef.h>

/**
 * Size in bytes of a buffer for `error_system_message`, including the `NUL` terminator. It holds
 * any message the C library spells for an `errno`. Diagnostics themselves go to a `struct Log`,
 * which has no fixed size.
 */
enum { ERROR_SYSTEM_MESSAGE_SIZE = 256 };

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
