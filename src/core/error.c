#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "core/error.h"

#include <stdio.h>
#include <string.h>

const char* error_system_message(char* message, size_t message_len, int error_number) {
  // This is the thread-safe variant. `strerror` shares one buffer across threads, and render jobs
  // report filesystem failures concurrently.
  if (strerror_r(error_number, message, message_len) != 0) {
    (void)snprintf(message, message_len, "system error %d", error_number);
  }
  return message;
}
