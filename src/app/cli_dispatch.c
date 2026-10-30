#include "app/cli_dispatch.h"

#include <stdlib.h>

#include "app/cli.h"
#include "app/cmd_build.h"
#include "app/cmd_config.h"
#include "app/exit_code.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/str.h"

/**
 * @brief Runs the command an accepted command line selected. A step of `cli_dispatch`.
 *
 * @param options Parsed command line, whose action is `CLI_ACTION_RUN`. Must not be `NULL`.
 * @param os      Host the command runs against. Must not be `NULL`.
 * @return The exit code of the command.
 */
static enum ExitCode cli_dispatch_command(const struct CliOptions* options, struct Os* os)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Prints what a command line that runs no command asked for: the version, the help, or the
 *        diagnostic of a rejected command line. A step of `cli_dispatch`.
 *
 * @param options Parsed command line, whose action is not `CLI_ACTION_RUN`. Must not be `NULL`.
 * @param argv    Argument vector `options` was parsed from. Must not be `NULL`.
 * @param os      Host to print on. Must not be `NULL`.
 * @param scratch Arena the help text is formatted in.
 * @return The exit code of the action.
 */
static enum ExitCode cli_dispatch_print(const struct CliOptions* options,
                                        char* const* argv,
                                        struct Os* os,
                                        struct Arena scratch) __attribute__((nonnull(1, 2, 3)));

enum ExitCode cli_dispatch(int argc, char** argv, struct Os* os) {
  // The command line's diagnostic quotes an argument, which only `ARG_MAX` bounds, so it is
  // formatted into a reservation of its own that holds several times the largest `ARG_MAX` in use.
  // Under an address-space limit the reservation is halved, and the version and the help text still
  // fit.
  enum { CLI_DISPATCH_RESERVE_LEN = 16 * 1024 * 1024 };
  struct Arena block;
  (void)arena_reserve_halving(CLI_DISPATCH_RESERVE_LEN, &block);
  struct Arena arena = block;
  struct CliOptions options;
  cli_parse(&options, argc, argv, &arena);
  // A command reserves its own memory. Everything else prints from this reservation: the diagnostic
  // `cli_parse` formatted there, or the help text, which quotes `argv[0]`.
  const enum ExitCode rc = options.action == CLI_ACTION_RUN
                               ? cli_dispatch_command(&options, os)
                               : cli_dispatch_print(&options, argv, os, arena);
  arena_release(block);
  return rc;
}

static enum ExitCode cli_dispatch_command(const struct CliOptions* options, struct Os* os) {
  switch (options->command) {
    case CLI_COMMAND_BUILD: {
      const struct BuildOptions build_options = {
          .worker_count = options->worker_count,
          .is_verbose = options->is_verbose,
          .is_dry_run = options->is_dry_run,
          .is_forced = options->is_forced,
      };
      return cmd_build_run(&build_options, os);
    }
    case CLI_COMMAND_CONFIG:
      return cmd_config_run(os);
    case CLI_COMMAND_NONE:
      break;
  }
  // Unreachable: `cli_parse` only resolves `CLI_ACTION_RUN` alongside a real command, and the
  // switch covers every `enum CliCommand`. This is deliberately not a `default:` case, so
  // `-Wswitch` still fails the build when someone adds a command without a case here. A value
  // outside the enum means memory corruption, which there is no sensible exit code for.
  abort();
}

static enum ExitCode cli_dispatch_print(const struct CliOptions* options,
                                        char* const* argv,
                                        struct Os* os,
                                        struct Arena scratch) {
  switch (options->action) {
    case CLI_ACTION_VERSION:
      // A failed write, such as to a closed `stdout`, exits with `EXIT_CODE_FAILURE`.
      return cli_print_version(os, scratch) == 0 ? EXIT_CODE_OK : EXIT_CODE_FAILURE;
    case CLI_ACTION_HELP: {
      // `CLI_ACTION_HELP` means `cli_parse` saw `--help`, so `argc >= 2` and `argv[0]` is a string.
      // The `argc == 0` case that ISO C permits never arrives here. With no arguments no command
      // resolves, and `cli_parse` returns `CLI_ACTION_ERROR` instead.
      const struct Str program_name = str_from_cstr(argv[0]);
      return cli_print_usage(program_name, options->command, os, scratch) == 0 ? EXIT_CODE_OK
                                                                               : EXIT_CODE_FAILURE;
    }
    case CLI_ACTION_ERROR:
      // A diagnostic that cannot be printed has nowhere else to go, and the exit code still says
      // the command line was rejected.
      (void)os_print(os, 2, options->error_message);
      (void)os_print(os, 2, S("\n"));
      return EXIT_CODE_USAGE;
    case CLI_ACTION_RUN:
      break;
  }
  // Unreachable: `cli_dispatch` passes a command line that runs a command to `cli_dispatch_command`
  // instead. This is deliberately not a `default:` case, so `-Wswitch` still fails the build when
  // someone adds an action without a case here.
  abort();
}
