#ifndef FRAM_CLI_DISPATCH_H
#define FRAM_CLI_DISPATCH_H

#include "app/exit_code.h"

/**
 * @brief Parses the command line, then runs the requested action or command.
 *
 * Any diagnostic goes to `stderr`. When this function cannot write the version or usage text, it
 * reports the stream's `errno` as the cause. A full disk and a closed or invalid descriptor need
 * different responses. An exit code alone cannot tell those two causes apart.
 *
 * @param argc Argument count.
 * @param argv Argument vector. Must not be `NULL`.
 * @return `EXIT_CODE_OK` on success, `EXIT_CODE_USAGE` on a command-line error, or
 *         `EXIT_CODE_FAILURE` when the selected command fails or the version or help text cannot be
 *         written. Does not return when the parse result is internally inconsistent.
 */
enum ExitCode cli_dispatch(int argc, char** argv) __attribute__((nonnull(2)));

#endif
