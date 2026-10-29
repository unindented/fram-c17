#ifndef FRAM_CLI_H
#define FRAM_CLI_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/str.h"

struct Arena;
struct Os;

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
   * because `cli_parse_workers_option` rejects it, so it serves as the unset sentinel. The build
   * caps the count at four per online CPU. `BuildOptions.worker_count` documents `0` the same way.
   */
  ptrdiff_t worker_count;

  /** Whether the caller requested progress and status messages. */
  bool is_verbose;

  /** Whether the build only reports what it would write, keep, or remove, changing nothing. */
  bool is_dry_run;

  /**
   * Failure diagnostic, non-empty exactly when `action` is `CLI_ACTION_ERROR`. It is a formatted
   * copy in the arena `cli_parse` was given, so it holds no pointer into `argv` or into copt's
   * scratch storage.
   */
  struct Str error_message;
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
 * reorders or overwrites `argv`. `error_message` is a formatted copy in `arena`, so the diagnostic
 * does not retain a pointer into copt's scratch storage either. A short option's spelling lives in
 * the parser that `cli_parse` keeps on its own stack, so a later change to avoid the copy would
 * hand back a pointer that dangles as soon as `cli_parse` returns.
 *
 * @param options Receives the fully resolved parse result. Must not be `NULL`.
 * @param argc    Argument count.
 * @param argv    Argument vector. Elements after `argv[0]` may be reordered. Must not be `NULL`.
 * @param arena   Arena that owns `error_message`. Must not be `NULL`.
 */
void cli_parse(struct CliOptions* options, int argc, char** argv, struct Arena* arena)
    __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Writes the `fram <version>` line to standard output.
 *
 * The line is formatted whole and printed in one `os_print` call.
 *
 * @param os      Host to print on. Must not be `NULL`.
 * @param scratch Arena the line is formatted in.
 * @return `0` on success, or `-1` when the write fails.
 */
int cli_print_version(struct Os* os, struct Arena scratch) __attribute__((nonnull(1)));

/**
 * @brief Writes usage and option help for `command` to standard output.
 *
 * Prints command-specific help when a command is selected, otherwise the top-level summary. The
 * text is formatted whole and printed in one `os_print` call.
 *
 * @param program_name Program name to show in usage lines, such as `argv[0]`.
 * @param command      Command whose help to print, or `CLI_COMMAND_NONE` for the summary.
 * @param os           Host to print on. Must not be `NULL`.
 * @param scratch      Arena the text is formatted in. The program name is bounded only by
 *                     `ARG_MAX`, so it must hold several times that.
 * @return `0` on success, or `-1` when the write fails.
 */
int cli_print_usage(struct Str program_name,
                    enum CliCommand command,
                    struct Os* os,
                    struct Arena scratch) __attribute__((nonnull(3)));

#endif
