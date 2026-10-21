#ifndef FRAM_CLI_DISPATCH_H
#define FRAM_CLI_DISPATCH_H

#include "app/exit_code.h"

struct Os;

/**
 * @brief Parses the command line, then runs the requested action or command.
 *
 * Any diagnostic goes to standard error through `os`. When this function cannot write the version
 * or usage text, it returns `EXIT_CODE_FAILURE` and prints nothing.
 *
 * @param argc Argument count.
 * @param argv Argument vector. Must not be `NULL`.
 * @param os   Host the selected command runs against and every message is printed on. Must not be
 *             `NULL`.
 * @return `EXIT_CODE_OK` on success, `EXIT_CODE_USAGE` on a command-line error, or
 *         `EXIT_CODE_FAILURE` when the selected command fails or the version or help text cannot be
 *         written. Does not return when the parse result is internally inconsistent.
 */
enum ExitCode cli_dispatch(int argc, char** argv, struct Os* os) __attribute__((nonnull(2, 3)));

#endif
