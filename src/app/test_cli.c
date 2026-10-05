#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/cli.h"
#include "core/fram_version.h"

/**
 * @brief Parses a terminated argument vector.
 *
 * @param argv `NULL`-terminated argument vector. Must not be `NULL`.
 * @return The resolved CLI options.
 */
static struct CliOptions parse(char** argv) {
  int argc = 0;
  while (argv[argc] != NULL) {
    argc++;
  }
  struct CliOptions options;
  cli_parse(&options, argc, argv);
  return options;
}

// The `build` command resolves to the run action with no worker override.
static void test_build_command_runs(void) {
  char* argv[] = {"fram", "build", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.command == CLI_COMMAND_BUILD);
  TEST_CHECK(options.worker_count == 0);
}

// The `config` command resolves to the run action.
static void test_config_command_runs(void) {
  char* argv[] = {"fram", "config", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.command == CLI_COMMAND_CONFIG);
}

// `--verbose` enables verbose output.
static void test_verbose_long_flag(void) {
  char* argv[] = {"fram", "build", "--verbose", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_verbose);
}

// `-v` enables verbose output.
static void test_verbose_short_flag(void) {
  char* argv[] = {"fram", "build", "-v", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_verbose);
}

// `--workers 4` sets the worker count from a separate token.
static void test_workers_long_flag_separate_value(void) {
  char* argv[] = {"fram", "build", "--workers", "4", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 4);
}

// `-w 8` sets the worker count from a separate token.
static void test_workers_short_flag_separate_value(void) {
  char* argv[] = {"fram", "build", "-w", "8", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 8);
}

// `-w4` sets the worker count from a value attached to the flag.
static void test_workers_short_flag_attached_value(void) {
  char* argv[] = {"fram", "build", "-w4", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 4);
}

// `--workers=16` sets the worker count from an `=`-joined value.
static void test_workers_long_flag_equals_value(void) {
  char* argv[] = {"fram", "build", "--workers=16", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 16);
}

// An `=`-joined value inside a short cluster reaches the one flag it is attached to, leaving the
// valueless flag ahead of it alone. This is the case the rejection in
// `test_attached_value_rejected_on_valueless_short_flags` must not claim. Both flags live in one
// `argv` element, so a check that only asked whether that element contains an `=` would reject
// `--verbose` here for a value belonging to `--workers`.
static void test_workers_short_flag_in_cluster_equals_value(void) {
  char* argv[] = {"fram", "build", "-vw=4", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_verbose);
  TEST_CHECK(options.worker_count == 4);
}

// A command flag before the command and another after it are both applied in one parse. The
// all-flags-after-the-command placement is `test_options_after_build_command`.
static void test_mixed_flag_order(void) {
  char* argv[] = {"fram", "-v", "build", "--workers", "2", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.command == CLI_COMMAND_BUILD);
  TEST_CHECK(options.is_verbose);
  TEST_CHECK(options.worker_count == 2);
}

// `argv[0]` survives a parse that permutes the rest of the vector, which is the clause
// `cli_dispatch` depends on. It reads `argv[0]` after `cli_parse` returns to name the program in
// the usage text. This command line reorders because the option follows the positional. The test
// fails if a copt update rotates the complete vector. It also fails if `copt_init` stops preserving
// `argv[0]`.
static void test_program_name_survives_permutation(void) {
  char* argv[] = {"fram", "build", "--workers", "2", NULL};
  char* program_name = argv[0];
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 2);
  TEST_CHECK(argv[0] == program_name);
  TEST_CHECK(strcmp(argv[0], "fram") == 0);
}

// Flags placed after the command are still parsed.
static void test_options_after_build_command(void) {
  char* argv[] = {"fram", "build", "-w", "3", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 3);
}

// `--help` with no command resolves to the help action with no command selected.
static void test_help_long_flag(void) {
  char* argv[] = {"fram", "--help", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.command == CLI_COMMAND_NONE);
}

// `-h` with no command resolves to the help action with no command selected.
static void test_help_short_flag(void) {
  char* argv[] = {"fram", "-h", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.command == CLI_COMMAND_NONE);
}

// `build --help` resolves to the help action scoped to the build command.
static void test_build_command_help(void) {
  char* argv[] = {"fram", "build", "--help", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.command == CLI_COMMAND_BUILD);
}

// `config --help` resolves to the help action scoped to the config command.
static void test_config_command_help(void) {
  char* argv[] = {"fram", "config", "--help", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.command == CLI_COMMAND_CONFIG);
}

// `--version` resolves to the version action.
static void test_version_long_flag(void) {
  char* argv[] = {"fram", "--version", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message[0] == '\0');
}

// `-V` resolves to the version action.
static void test_version_short_flag(void) {
  char* argv[] = {"fram", "-V", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
}

// Version wins over help regardless of their order.
static void test_version_flag_wins_over_help(void) {
  char* help_first[] = {"fram", "--help", "--version", NULL};
  struct CliOptions options = parse(help_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);

  char* version_first[] = {"fram", "--version", "--help", NULL};
  options = parse(version_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
}

// An informational flag takes priority over a parse error, and discards the diagnostic that error
// recorded, so `error_message` never describes an outcome other than the one selected. The cluster
// case is here because it looks like an exception and is not. `-Vx` is `-V` alongside the unknown
// option `-x`, the same shape as the two spelled-out cases above, so it prints the version rather
// than reporting the `-x`. A value glued to the flag itself is the shape that does not survive this
// precedence, per `test_attached_value_rejected_on_valueless_short_flags`.
static void test_informational_flag_clears_diagnostic(void) {
  char* help_argv[] = {"fram", "--frobnicate", "--help", NULL};
  struct CliOptions options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.error_message[0] == '\0');

  char* version_argv[] = {"fram", "--frobnicate", "--version", NULL};
  options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message[0] == '\0');

  char* cluster_argv[] = {"fram", "-Vx", NULL};
  options = parse(cluster_argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message[0] == '\0');
}

// A valid `--version` wins in either order next to a rejected `--version=1`. The rejected spelling
// neither requests the version nor cancels the valid request, so the result does not depend on
// which comes last.
static void test_valid_version_flag_wins_over_rejected_spelling(void) {
  char* valid_first[] = {"fram", "--version", "--version=1", NULL};
  struct CliOptions options = parse(valid_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message[0] == '\0');

  char* rejected_first[] = {"fram", "--version=1", "--version", NULL};
  options = parse(rejected_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message[0] == '\0');
}

// A non-numeric worker count is rejected with a diagnostic naming the offending value.
static void test_invalid_worker_count_rejected(void) {
  char* argv[] = {"fram", "build", "-w", "abc", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      strcmp(options.error_message, "option '--workers' must be a positive integer: 'abc'") == 0);
}

// A worker count of zero is rejected, naming the offending value.
static void test_zero_worker_count_rejected(void) {
  char* argv[] = {"fram", "build", "--workers", "0", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '--workers' must be a positive integer: '0'") ==
             0);
}

// A negative worker count in attached form reaches `parse_size`'s sign rejection, and the
// diagnostic names the offending value. Attached form (`=`) is required so the `-1` is taken as the
// option's value rather than a separate token.
static void test_negative_worker_count_rejected(void) {
  char* argv[] = {"fram", "build", "--workers=-1", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '--workers' must be a positive integer: '-1'") ==
             0);
}

// A worker count above the accepted ceiling is rejected. The value at the ceiling is accepted, so
// the boundary itself is pinned rather than just the rejection. `WORKER_COUNT_MAX` is file-local to
// `cli.c`, so the limit is spelled here as a literal. Changing it must update these two cases and
// the expected message below.
static void test_oversize_worker_count_rejected(void) {
  char* argv[] = {"fram", "build", "--workers", "1025", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message,
                    "option '--workers' exceeds max worker count (1024) at 1025") == 0);

  char* at_limit[] = {"fram", "build", "--workers", "1024", NULL};
  options = parse(at_limit);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 1024);
  TEST_CHECK(options.error_message[0] == '\0');
}

// `-w` with no following value is rejected with a diagnostic that it requires a worker count,
// naming the short spelling.
static void test_missing_worker_count_short_flag(void) {
  char* argv[] = {"fram", "build", "-w", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '-w' requires a worker count") == 0);
}

// `--workers` with no following value is rejected with a diagnostic that it requires a worker
// count, naming the long spelling.
static void test_missing_worker_count_long_flag(void) {
  char* argv[] = {"fram", "build", "--workers", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '--workers' requires a worker count") == 0);
}

// A flag that takes no value rejects an attached `=value` and names the complete token. copt
// matches only the text before `=`. Without this check, it discards the value and enables
// `--verbose=0`. The test covers all three flags that take no value. `--version` and `--help` clear
// `error_message`, so they could otherwise hide this rejection. The
// `test_workers_long_flag_equals_value` test confirms that `--workers=N` still works.
static void test_attached_value_rejected_on_valueless_flags(void) {
  char* verbose_argv[] = {"fram", "build", "--verbose=0", NULL};
  struct CliOptions options = parse(verbose_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_verbose);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '--verbose=0'") == 0);

  char* help_argv[] = {"fram", "--help=nope", NULL};
  options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '--help=nope'") == 0);

  char* version_argv[] = {"fram", "--version=nope", NULL};
  options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '--version=nope'") == 0);
}

// The short form of each valueless flag rejects an attached value and names the complete element.
// copt stops its one-letter comparison at `=`, so `-V=1` matches `V`. Without this check, the short
// form would print the version while the long form failed. An informational flag clears
// `error_message`, so the rejection must prevent `-V` from becoming a version request.
static void test_attached_value_rejected_on_valueless_short_flags(void) {
  char* verbose_argv[] = {"fram", "build", "-v=0", NULL};
  struct CliOptions options = parse(verbose_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_verbose);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-v=0'") == 0);

  char* help_argv[] = {"fram", "-h=1", NULL};
  options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-h=1'") == 0);

  char* version_argv[] = {"fram", "-V=1", NULL};
  options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-V=1'") == 0);
}

// A rejected attached value ends its short cluster, so the letters after the `=` are not read as
// flags. Otherwise the `V` in `-v=V` would request the version and the `h` in `-v=h` would request
// help, and either would clear the rejection.
static void test_attached_value_rejection_ends_short_cluster(void) {
  char* version_argv[] = {"fram", "-v=V", NULL};
  struct CliOptions options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_verbose);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-v=V'") == 0);

  char* help_argv[] = {"fram", "-v=h", NULL};
  options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-v=h'") == 0);
}

// An unrecognized short option is rejected with a diagnostic naming the option.
static void test_unknown_short_option(void) {
  char* argv[] = {"fram", "build", "-x", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "unknown option '-x'") == 0);
}

// A value attached to an unknown short option ends its cluster, so the `V` in `-x=V` does not
// request the version. The diagnostic names the unknown option rather than the value.
static void test_unknown_short_option_value_ends_cluster(void) {
  char* argv[] = {"fram", "build", "-x=V", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "unknown option '-x'") == 0);
}

// An unrecognized long option is rejected with a diagnostic naming the option.
static void test_unknown_long_option(void) {
  char* argv[] = {"fram", "build", "--frobnicate", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "unknown option '--frobnicate'") == 0);
}

// No arguments at all are reported as an error with a diagnostic message.
static void test_no_args_rejected(void) {
  char* argv[] = {"fram", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "no command specified") == 0);
}

// The config command rejects a build-only flag like `-v`, naming the command and the flag.
static void test_config_command_rejects_verbose(void) {
  char* argv[] = {"fram", "config", "-v", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "command 'config' does not accept option '--verbose'") ==
             0);
}

// The config command rejects a build-only flag like `-w`, naming the command and the flag.
static void test_config_command_rejects_workers(void) {
  char* argv[] = {"fram", "config", "-w", "4", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "command 'config' does not accept option '--workers'") ==
             0);
}

// With two unsupported flags, the reported one is fixed by the `cli_flags` table order rather than
// by the order the user typed them. That is the claim the table's own comment makes, and what
// `reject_unsupported_flags` returning after the first match implements. Both input orders are
// asserted because correct behavior must not depend on an unspecified detail. Each single-flag test
// above passes whichever entry comes first.
static void test_config_command_rejects_first_flag_in_table_order(void) {
  char* verbose_first[] = {"fram", "config", "-v", "-w", "4", NULL};
  struct CliOptions options = parse(verbose_first);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "command 'config' does not accept option '--verbose'") ==
             0);

  char* workers_first[] = {"fram", "config", "-w", "4", "-v", NULL};
  options = parse(workers_first);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "command 'config' does not accept option '--verbose'") ==
             0);
}

// An unrecognized command is rejected with a diagnostic naming the command.
static void test_unknown_command(void) {
  char* argv[] = {"fram", "serve", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "unknown command 'serve'") == 0);
}

// A trailing positional argument the command does not accept is rejected, naming the argument.
static void test_extra_positional_argument(void) {
  char* argv[] = {"fram", "build", "extra", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "unexpected argument 'extra'") == 0);
}

// The generated version accessor returns a non-empty version.
static void test_version_accessor_present(void) {
  TEST_CHECK(fram_version_string()[0] != '\0');
}

// `cli_print_version` writes exactly one `fram <version>` line and nothing else.
static void test_print_version_writes_version_line(void) {
  char* buf = NULL;
  size_t len = 0;
  FILE* stream = open_memstream(&buf, &len);
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  TEST_CHECK(cli_print_version(stream) == 0);
  const int rc = fclose(stream);
  TEST_ASSERT(rc == 0);

  char expected[64];
  const int expected_len = snprintf(expected, sizeof(expected), "fram %s\n", fram_version_string());
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(buf, expected) == 0);
  TEST_CHECK(len == strlen(expected));
  free(buf);
}

// A stream that latched a write error makes `cli_print_version` report `-1` with `errno` set, which
// lets `cli_dispatch` name a reason rather than exiting silently. The reason is `EIO` rather than
// the underlying `EBADF`. A latched error's own `errno` may have been overwritten by the time it is
// noticed, so the contract substitutes a generic I/O failure instead of relaying a stale value. A
// read-mode stream is the deterministic way to reach that branch, because the write fails at the
// `fprintf` while `fflush` itself succeeds.
static void test_print_version_reports_write_failure(void) {
  FILE* stream = fopen("/dev/null", "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  errno = 0;
  TEST_CHECK(cli_print_version(stream) == -1);
  TEST_CHECK(errno == EIO);
  const int close_rc = fclose(stream);
  TEST_CHECK(close_rc == 0);
}

// When `fflush` itself fails, the write's own `errno` survives instead of being replaced by the
// `EIO` the latched-error branch substitutes. That distinction is why `cli_dispatch` reports a
// reason rather than only an exit code. A closed pipe and a full disk need different responses. The
// test above reaches the `ferror` branch, where the write already failed at `fprintf` and `fflush`
// succeeds. This one reaches the other branch. A pipe with its read end closed is the deterministic
// way there, with `SIGPIPE` ignored so the process survives to return.
static void test_print_version_reports_flush_failure_errno(void) {
  void (*previous)(int) = signal(SIGPIPE, SIG_IGN);
  int fds[2];
  TEST_ASSERT(pipe(fds) == 0);
  TEST_ASSERT(close(fds[0]) == 0);
  FILE* stream = fdopen(fds[1], "w");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    (void)close(fds[1]);
    (void)signal(SIGPIPE, previous);
    return;
  }

  errno = 0;
  const int rc = cli_print_version(stream);
  const int reported = errno;
  // Once a flush has failed, libc implementations differ on whether `fclose` reports the stream's
  // existing error or succeeds because no buffered output remains. Either result closes it.
  (void)fclose(stream);
  (void)signal(SIGPIPE, previous);

  TEST_CHECK(rc == -1);
  // This is `EPIPE`, not the `EIO` substitute. `fflush` observed the failure, so its `errno` is
  // current and passes through untouched.
  TEST_CHECK(reported == EPIPE);
}

// `cli_print_usage` substitutes the caller's program name into the usage line, in the right
// position for the selected command, alongside command-specific option content. The name passed
// here is deliberately *not* `"fram"`. With the real program name the assertion passes just as well
// against a hard-coded usage line, so it would prove nothing about substitution. `cli_dispatch`
// passes `argv[0]`, so a user invoking `/usr/local/bin/fram build --help` has to see that path
// back. The full help text is prose and is deliberately not asserted whole. The usage line is the
// part callers copy.
static void test_print_usage_names_program_and_command(void) {
  static const char* const prog_name = "/opt/bin/myfram";
  const struct {
    enum CliCommand command;
    const char* usage_line;
    const char* needle;
  } cases[] = {
      {CLI_COMMAND_BUILD, "    /opt/bin/myfram build [options]\n", "-w, --workers N"},
      {CLI_COMMAND_CONFIG, "    /opt/bin/myfram config [options]\n", "-h, --help"},
      {CLI_COMMAND_NONE, "    /opt/bin/myfram <command> [options]\n", "-V, --version"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char* buf = NULL;
    size_t len = 0;
    FILE* stream = open_memstream(&buf, &len);
    TEST_ASSERT(stream != NULL);
    if (stream == NULL) {
      return;
    }
    TEST_CHECK(cli_print_usage(stream, prog_name, cases[i].command) == 0);
    const int rc = fclose(stream);
    TEST_ASSERT(rc == 0);
    TEST_CHECK(strstr(buf, cases[i].usage_line) != NULL);
    TEST_CHECK(strstr(buf, cases[i].needle) != NULL);
    // The command-less help substitutes the name a second time, in the trailer.
    if (cases[i].command == CLI_COMMAND_NONE) {
      TEST_CHECK(strstr(buf, "Use '/opt/bin/myfram <command> --help'") != NULL);
    }
    free(buf);
  }
}

// `cli_print_usage` reports a failed write the same way, so neither informational action can exit
// non-zero without a reason.
static void test_print_usage_reports_write_failure(void) {
  FILE* stream = fopen("/dev/null", "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  errno = 0;
  TEST_CHECK(cli_print_usage(stream, "fram", CLI_COMMAND_NONE) == -1);
  TEST_CHECK(errno == EIO);
  const int close_rc = fclose(stream);
  TEST_CHECK(close_rc == 0);
}

TEST_LIST = {
    {"build command runs", test_build_command_runs},
    {"config command runs", test_config_command_runs},
    {"verbose long flag", test_verbose_long_flag},
    {"verbose short flag", test_verbose_short_flag},
    {"workers long flag separate value", test_workers_long_flag_separate_value},
    {"workers short flag separate value", test_workers_short_flag_separate_value},
    {"workers short flag attached value", test_workers_short_flag_attached_value},
    {"workers long flag equals value", test_workers_long_flag_equals_value},
    {"workers short flag in cluster equals value", test_workers_short_flag_in_cluster_equals_value},
    {"mixed flag order", test_mixed_flag_order},
    {"program name survives permutation", test_program_name_survives_permutation},
    {"options after build command", test_options_after_build_command},
    {"help long flag", test_help_long_flag},
    {"help short flag", test_help_short_flag},
    {"build command help", test_build_command_help},
    {"config command help", test_config_command_help},
    {"version long flag", test_version_long_flag},
    {"version short flag", test_version_short_flag},
    {"version flag wins over help", test_version_flag_wins_over_help},
    {"informational flag clears diagnostic", test_informational_flag_clears_diagnostic},
    {"valid version flag wins over rejected spelling",
     test_valid_version_flag_wins_over_rejected_spelling},
    {"invalid worker count rejected", test_invalid_worker_count_rejected},
    {"zero worker count rejected", test_zero_worker_count_rejected},
    {"negative worker count rejected", test_negative_worker_count_rejected},
    {"oversize worker count rejected", test_oversize_worker_count_rejected},
    {"missing worker count short flag", test_missing_worker_count_short_flag},
    {"missing worker count long flag", test_missing_worker_count_long_flag},
    {"attached value rejected on valueless flags", test_attached_value_rejected_on_valueless_flags},
    {"attached value rejected on valueless short flags",
     test_attached_value_rejected_on_valueless_short_flags},
    {"attached value rejection ends short cluster",
     test_attached_value_rejection_ends_short_cluster},
    {"unknown short option", test_unknown_short_option},
    {"unknown short option value ends cluster", test_unknown_short_option_value_ends_cluster},
    {"unknown long option", test_unknown_long_option},
    {"no args rejected", test_no_args_rejected},
    {"config command rejects verbose", test_config_command_rejects_verbose},
    {"config command rejects workers", test_config_command_rejects_workers},
    {"config command rejects first flag in table order",
     test_config_command_rejects_first_flag_in_table_order},
    {"unknown command", test_unknown_command},
    {"extra positional argument", test_extra_positional_argument},
    {"version accessor present", test_version_accessor_present},
    {"print version writes version line", test_print_version_writes_version_line},
    {"print version reports write failure", test_print_version_reports_write_failure},
    {"print version reports flush failure errno", test_print_version_reports_flush_failure_errno},
    {"print usage names program and command", test_print_usage_names_program_and_command},
    {"print usage reports write failure", test_print_usage_reports_write_failure},
    {NULL, NULL},
};
