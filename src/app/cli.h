#ifndef FRAM_CLI_H
#define FRAM_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "core/error.h"

/** Action the parsed command line selected, resolved by precedence in `cli_parse`. */
enum CliAction {
  /** Print the version and exit successfully. Wins over every other flag. */
  CLI_ACTION_VERSION,

  /** Print usage help and exit successfully. Wins over parse errors but not version. */
  CLI_ACTION_HELP,

  /** Run the selected command. See `command` for which one. */
  CLI_ACTION_RUN,

  /** Reject the command line. `error_message` explains the failure. */
  CLI_ACTION_ERROR,
};

/** Subcommand selected on the command line. `CLI_COMMAND_NONE` until a valid command parses. */
enum CliCommand {
  /** No recognized command. Only version and help may run without one. */
  CLI_COMMAND_NONE,

  /** Build the gallery in the current directory. */
  CLI_COMMAND_BUILD,

  /** Print the resolved project configuration. */
  CLI_COMMAND_CONFIG,
};

/** Parsed command line for one fram invocation. */
struct CliOptions {
  /** Selected action after applying flag precedence. */
  enum CliAction action;

  /** Command the positional argument selected. It also selects the help or usage text. */
  enum CliCommand command;

  /**
   * Requested worker count, or `0` when `--workers` was not given. `0` is never a valid request,
   * because `cli_parse_workers_option` rejects it, so it serves as the unset sentinel. That
   * function also caps the accepted value, so a stored count is always one the pool can reasonably
   * run. `BuildOptions.worker_count` documents `0` the same way.
   */
  size_t worker_count;

  /** Whether the caller requested progress and status messages. */
  bool is_verbose;

  /** Failure diagnostic, non-empty when `action` is `CLI_ACTION_ERROR`. */
  char error_message[ERROR_MESSAGE_SIZE];
};

/**
 * @brief Parses `argv` into `options`, resolving the action by fixed precedence.
 *
 * Never prints or exits. The caller inspects `options->action` and acts on it. On a rejected
 * command line, `options->action` is `CLI_ACTION_ERROR` and `options->error_message` holds the
 * first diagnostic. May permute `argv[1..argc-1]` in place (it reorders options ahead of positional
 * arguments). It leaves `argv[0]` untouched.
 *
 * `options` retains no pointer into `argv`, so the result stays valid however the caller later
 * reorders or overwrites `argv`. `error_message` is a fixed inline buffer holding a formatted copy,
 * so the diagnostic itself does not retain a pointer into copt's scratch storage. That is also why
 * it is an array rather than a `const char*`. A short option's spelling lives in the parser that
 * `cli_parse` keeps on its own stack, so a later change to avoid the copy would hand back a pointer
 * that dangles as soon as `cli_parse` returns.
 *
 * @param options Receives the fully resolved parse result. Must not be `NULL`.
 * @param argc    Argument count.
 * @param argv    Argument vector. Elements after `argv[0]` may be reordered. Must not be `NULL`.
 */
void cli_parse(struct CliOptions* options, int argc, char** argv) __attribute__((nonnull(1, 3)));

/**
 * @brief Writes the `fram <version>` line to `stream`.
 *
 * @param stream Destination stream. Must not be `NULL`.
 * @return `0` on success, or `-1` if writing to `stream` failed, with `errno` set by the failing
 *         write, or to `EIO` when the stream had latched an error earlier and the original `errno`
 *         is no longer available, so the caller always has a reason to report.
 */
int cli_print_version(FILE* stream) __attribute__((nonnull(1)));

/**
 * @brief Writes usage and option help for `command` to `stream`.
 *
 * Prints command-specific help when a command is selected, otherwise the top-level summary.
 *
 * @param stream       Destination stream. Must not be `NULL`.
 * @param program_name Program name to show in usage lines. Must not be `NULL`.
 * @param command      Command whose help to print, or `CLI_COMMAND_NONE` for the summary.
 * @return `0` on success, or `-1` if writing to `stream` failed, with `errno` set as for
 *         `cli_print_version`.
 */
int cli_print_usage(FILE* stream, const char* program_name, enum CliCommand command)
    __attribute__((nonnull(1, 2)));

#endif
