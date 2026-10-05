#include "app/cli.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>

// copt is header-only. This is the single translation unit that defines its implementation. A
// second definition of `COPT_IMPL` would duplicate every copt symbol at link time.
#define COPT_IMPL
#include <copt.h>

#include "core/fram_version.h"
#include "core/parse.h"

/**
 * Largest worker count `--workers` accepts.
 *
 * The flag needs a ceiling of its own, because `pool_run` clamps only from below (`0` becomes `1`)
 * and against the job count. The job count is the number of media items for the probe and
 * derivative pools and the number of albums for the album-render pool, and neither is bounded.
 * Without a cap, `--workers` is bounded only by how much media a gallery has. Each worker is a
 * thread with a stack, and the work it drives is CPU bound: decoding, resizing, and encoding JPEG
 * in process, or an `ffmpeg` or `ffprobe` subprocess competing for the same cores. Workers beyond
 * the core count add threads, stacks, and subprocesses without adding throughput.
 *
 * 1024 is deliberately unreachable by legitimate use rather than tuned to an optimum. It sits well
 * above the thread count of the largest machine this tool will plausibly run on, so no real
 * `--workers` value meets it. A pathological value fails with a diagnostic rather than thrashing.
 * The tool deliberately does not cap the detected default. It is the machine's own core count,
 * which is a physical bound rather than user input, and a large host should use it.
 */
enum { WORKER_COUNT_MAX = 1024 };

/**
 * Command-scoped optional flags, tracked as a bitmask so each command can declare which it accepts.
 * Global flags (`--help`, `--version`) apply to every command, so this enum does not model them.
 */
enum CliFlag {
  CLI_FLAG_VERBOSE = 1u << 0,
  CLI_FLAG_WORKERS = 1u << 1,
};

/** One command-scoped flag paired with the long-option name used to name it in diagnostics. */
struct CliFlagSpec {
  unsigned bit;
  const char* name;
};

/** Static description of one subcommand: its name and the command-scoped flags it accepts. */
struct CliCommandSpec {
  const char* name;
  unsigned flags_accepted;
};

/**
 * Command-scoped flags in the order the code checks them. This order fixes which flag a command
 * reports when it rejects several.
 */
static const struct CliFlagSpec cli_flags[] = {
    {CLI_FLAG_VERBOSE, "--verbose"},
    {CLI_FLAG_WORKERS, "--workers"},
};

/** Subcommand table indexed by `enum CliCommand`. `CLI_COMMAND_NONE` stays zero-initialized. */
static const struct CliCommandSpec cli_commands[] = {
    [CLI_COMMAND_BUILD] = {"build", CLI_FLAG_VERBOSE | CLI_FLAG_WORKERS},
    [CLI_COMMAND_CONFIG] = {"config", 0},
};

/**
 * @brief Reports whether a parse error has already been recorded.
 *
 * @param options Options whose `error_message` is inspected. Must not be `NULL`.
 * @return `true` when a diagnostic has been recorded, `false` otherwise.
 */
static bool has_error(const struct CliOptions* options) __attribute__((nonnull(1)));

/**
 * @brief Records the first parse error and marks the command line as rejected.
 *
 * This keeps only the first error, so the user sees the earliest failure. It ignores later calls
 * once `has_error` reports a recorded diagnostic.
 *
 * @param options Options whose `error_message` receives the diagnostic. Must not be `NULL`.
 * @param fmt     `printf`-style format string. Must not be `NULL`.
 * @param ...     Arguments for `fmt`.
 */
static void record_error(struct CliOptions* options, const char* fmt, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Reports a missing option argument as `NULL` instead of letting copt exit.
 *
 * This is installed as copt's no-arg callback. Returning `NULL` lets `cli_parse` report the missing
 * argument itself.
 *
 * @param opt Current copt option (unused).
 * @param aux Callback data (unused).
 * @return `NULL` always.
 */
static char* cli_parse_handle_missing_arg(const struct copt* opt, void* aux);

/**
 * @brief Requires that an option taking no value has none attached.
 *
 * copt matches a long option by the text before any `=`, so `--verbose=0` matches `verbose`. copt
 * then silently drops the `0`, because nothing asks for an argument the option does not have. A
 * user who asks for verbosity off would get it on. Nothing else reads the value, so nothing else
 * can notice it.
 *
 * The caller must branch on the return value, not just the recorded diagnostic. An informational
 * flag clears `error_message` by design, so a `--version=x` that still set `has_version` would have
 * its rejection wiped and exit `0`. Treating the malformed spelling as not a version request
 * prevents that.
 *
 * copt matches a short option the same way, clamping its one-letter comparison at an `=`, so `-V=1`
 * matches `V`. The check therefore inspects the raw argv element rather than `copt_curopt`, which
 * for a short option is copt's own two-byte `-x` scratch string and can never hold an `=`. Reading
 * only that would leave `-V=1` accepted as a version request while `--version=1` was rejected.
 *
 * A rejection also consumes the rest of the argv element. copt would otherwise go on reading the
 * letters after the `=` as a short cluster, so the `V` in `-v=V` would become a version request and
 * the informational flag would then clear the rejection.
 *
 * @param options Options that receive the diagnostic when a value is attached. Must not be `NULL`.
 * @param opt     copt parser positioned on the option to inspect, before any `copt_arg` call, which
 *                may advance the parser off the element being inspected. On rejection it is left at
 *                the end of that element. Must not be `NULL`.
 * @param argv    Argument vector the parser is walking, used to reach the current element whole.
 *                Must not be `NULL`.
 * @return `0` when no value is attached, or `-1` with a diagnostic recorded.
 */
static int cli_parse_require_no_attached_value(struct CliOptions* options,
                                               struct copt* opt,
                                               char** argv) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Parses and validates the `--workers` option argument.
 *
 * Records a diagnostic when the argument is missing, is not a positive integer, or exceeds
 * `WORKER_COUNT_MAX`. On success stores the worker count and marks the flag as seen.
 *
 * @param options    Options receiving the worker count or a diagnostic. Must not be `NULL`.
 * @param opt        copt parser positioned on the `--workers` option. Must not be `NULL`.
 * @param flags_seen Bitmask of `enum CliFlag` values updated with `CLI_FLAG_WORKERS` on success.
 *                   Must not be `NULL`.
 */
static void cli_parse_workers_option(struct CliOptions* options,
                                     struct copt* opt,
                                     unsigned* flags_seen) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Resolves the positional command argument and validates it against the seen flags.
 *
 * Does nothing when no positional argument is present. Otherwise sets `options->command` for a
 * recognized name, or records a diagnostic for an unknown command, an unexpected extra argument, or
 * a command-scoped flag the command does not accept.
 *
 * @param options          Options receiving the resolved command and any diagnostic. Must not be
 *                         `NULL`.
 * @param argc             Argument count.
 * @param argv             Argument vector. Must not be `NULL`.
 * @param positional_index Index of the first positional argument, from `copt_idx`.
 * @param flags_seen       Bitmask of `enum CliFlag` values that appeared on the command line.
 */
static void cli_parse_command(struct CliOptions* options,
                              int argc,
                              char** argv,
                              int positional_index,
                              unsigned flags_seen) __attribute__((nonnull(1, 3)));

/**
 * @brief Rejects any command-scoped flag the selected command does not accept.
 *
 * Reports the first offending flag by name, so it rejects `config --workers 4` while global flags
 * stay valid for every command. Does nothing when every seen flag is accepted. The caller must only
 * invoke this once a real command is selected, never for `CLI_COMMAND_NONE`.
 *
 * @param options    Options holding the resolved command. Receives the diagnostic on rejection.
 *                   Must not be `NULL`.
 * @param flags_seen Bitmask of `enum CliFlag` values that appeared on the command line.
 */
static void reject_unsupported_flags(struct CliOptions* options, unsigned flags_seen)
    __attribute__((nonnull(1)));

/**
 * @brief Flushes `stream` and reports whether any write to it failed, leaving a reason in `errno`.
 *
 * The check needs both halves. `fflush` reports a write that fails now, when the buffered bytes
 * reach the stream. `ferror` catches one the stream latched during an earlier unchecked `fprintf`.
 * Neither alone suffices, because `stdout` to a pipe or a file is fully buffered, so an earlier
 * `fprintf` can succeed while the write does not.
 *
 * The function always leaves `errno` describing the failure, which lets `cli_dispatch` report a
 * reason rather than only an exit code. A latched error's own `errno` may have been overwritten
 * since, so that case substitutes `EIO` rather than relay a stale value as the cause.
 * `gallery_config_print` documents and does the same.
 *
 * @param stream Stream to flush and inspect. Must not be `NULL`.
 * @return `0` when every write succeeded, or `-1` with `errno` set to the reason.
 */
static int flush_stream(FILE* stream) __attribute__((nonnull(1)));

void cli_parse(struct CliOptions* options, int argc, char** argv) {
  *options = (struct CliOptions){.action = CLI_ACTION_RUN};

  bool has_version = false;
  bool has_help = false;
  unsigned flags_seen = 0;

  struct copt opt = copt_init(argc, argv, 1);  // 1 enables argv reordering, not a start index
  copt_set_noargfn(&opt, cli_parse_handle_missing_arg, NULL);
  while (copt_next(&opt)) {
    // A valid informational flag stays requested whatever follows it, and a rejected spelling never
    // requests one, so the outcome does not depend on the order the flags appear in.
    if (copt_opt(&opt, "V|version")) {
      if (cli_parse_require_no_attached_value(options, &opt, argv) == 0) {
        has_version = true;
      }
    } else if (copt_opt(&opt, "h|help")) {
      if (cli_parse_require_no_attached_value(options, &opt, argv) == 0) {
        has_help = true;
      }
    } else if (copt_opt(&opt, "v|verbose")) {
      if (cli_parse_require_no_attached_value(options, &opt, argv) == 0) {
        options->is_verbose = true;
        flags_seen |= CLI_FLAG_VERBOSE;
      }
    } else if (copt_opt(&opt, "w|workers")) {
      cli_parse_workers_option(options, &opt, &flags_seen);
    } else {
      record_error(options, "unknown option '%s'", copt_curopt(&opt));
      // A value attached to an unknown option is dropped with the rest of its element, so `-x=V`
      // cannot request the version. The unknown-option diagnostic above is the one that stays,
      // because `record_error` keeps the first.
      (void)cli_parse_require_no_attached_value(options, &opt, argv);
    }
  }

  cli_parse_command(options, argc, argv, copt_idx(&opt), flags_seen);

  // Version wins over everything. Help then beats a genuine parse error. The parser rejects a bare
  // invocation with no command once no informational flag applies.
  if (has_version || has_help) {
    // An informational action replaces any diagnostic recorded above, so drop it rather than hand
    // back a message that does not describe the outcome. `error_message` is then non-empty exactly
    // when the action is `CLI_ACTION_ERROR`.
    options->error_message[0] = '\0';
    options->action = has_version ? CLI_ACTION_VERSION : CLI_ACTION_HELP;
  } else if (has_error(options)) {
    options->action = CLI_ACTION_ERROR;
  } else if (options->command == CLI_COMMAND_NONE) {
    record_error(options, "no command specified");
    options->action = CLI_ACTION_ERROR;
  }
}

int cli_print_version(FILE* stream) {
  fprintf(stream, "%s\n", fram_generator_string());
  return flush_stream(stream);
}

int cli_print_usage(FILE* stream, const char* program_name, enum CliCommand command) {
  switch (command) {
    case CLI_COMMAND_BUILD:
      fprintf(stream,
              "Build the gallery in the current directory.\n"
              "\n"
              "USAGE:\n"
              "    %s build [options]\n"
              "\n"
              "OPTIONS:\n"
              "    -w, --workers N  Render with N worker threads (default: detected CPU count)\n"
              "    -v, --verbose    Print build progress to stderr\n"
              "    -h, --help       Print this help and exit\n",
              program_name);
      break;
    case CLI_COMMAND_CONFIG:
      fprintf(stream,
              "Print the resolved project configuration as TOML.\n"
              "\n"
              "USAGE:\n"
              "    %s config [options]\n"
              "\n"
              "OPTIONS:\n"
              "    -h, --help  Print this help and exit\n",
              program_name);
      break;
    case CLI_COMMAND_NONE:
      fprintf(stream,
              "A static photo and video gallery generator.\n"
              "\n"
              "USAGE:\n"
              "    %s <command> [options]\n"
              "\n"
              "COMMANDS:\n"
              "    build   Build the gallery in the current directory\n"
              "    config  Print the resolved project configuration\n"
              "\n"
              "OPTIONS:\n"
              "    -h, --help     Print this help and exit\n"
              "    -V, --version  Print version information and exit\n"
              "\n"
              "Use '%s <command> --help' for more information about a command.\n",
              program_name, program_name);
      break;
  }
  // The `fflush` plus `ferror` pair, for the reason given on `flush_stream`.
  return flush_stream(stream);
}

static bool has_error(const struct CliOptions* options) {
  return options->error_message[0] != '\0';
}

static void record_error(struct CliOptions* options, const char* fmt, ...) {
  if (has_error(options)) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  error_report_va(options->error_message, sizeof(options->error_message), fmt, ap);
  va_end(ap);
}

static char* cli_parse_handle_missing_arg(const struct copt* opt, void* aux) {
  (void)opt;
  (void)aux;
  return NULL;
}

static int cli_parse_require_no_attached_value(struct CliOptions* options,
                                               struct copt* opt,
                                               char** argv) {
  const char* token = copt_curopt(opt);
  // The whole element, which for a short option is the entire cluster rather than the one letter
  // `token` names. copt documents `copt_idx` for use after the option loop, where it gives the
  // first positional argument. Called during the loop it reports the element being parsed, which is
  // the one needed here. copt's own contract does not promise that though.
  const char* element = argv[copt_idx(opt)];
  const char* equals = strchr(element, '=');
  if (equals == NULL) {
    return 0;
  }
  // Within a short cluster the value belongs to the single letter directly before the `=`, so
  // `-vw=4` is `--verbose` followed by a worker count rather than a value glued to `--verbose`.
  // Every earlier letter in the cluster is a separate option that received no value. A long option
  // owns any `=` in its own element, so it needs no such test.
  const bool is_short_option = element[1] != '-';
  if (is_short_option && equals[-1] != token[1]) {
    return 0;
  }
  // The whole element trails the cause. It is a raw argument, so it is unbounded and must not be
  // able to truncate the reason away.
  record_error(options, "option does not take a value: '%s'", element);
  // `copt_oarg` consumes whatever follows the current option in its element and ends any short
  // cluster, so the next `copt_next` moves on to the following element. The value must be attached
  // here, so it never reaches for the next element or calls the missing-argument handler. Like
  // `copt_arg`, it does not touch `curopt`, and `token` is not read after it.
  (void)copt_oarg(opt);
  return -1;
}

static void cli_parse_workers_option(struct CliOptions* options,
                                     struct copt* opt,
                                     unsigned* flags_seen) {
  const char* value = copt_arg(opt);
  size_t count = 0;
  if (value == NULL) {
    // `copt_curopt` returns a pointer into `opt`'s own scratch storage for a short option. That
    // pointer is valid only until the next `copt_next`, so the code must format it before the loop
    // advances. `record_error` copies into `options->error_message` on the spot. This is also the
    // one site that reaches `copt_curopt` after a copt call that can mutate the parser, namely
    // `copt_arg` on the line above. The `copt_opt` tests in `cli_parse` take a `const struct copt*`
    // and so cannot invalidate `curopt`. `copt_arg` takes a mutable one and is safe here only
    // because it never touches `curopt`. copt's own contract does not promise that, so recheck it
    // on a vendor bump. `cli_parse_require_no_attached_value` also calls a mutating `copt_oarg`,
    // but only after its last read of `curopt`, so it does not depend on that.
    //
    // A negative value also lands here rather than in the positive-integer check below. copt reads
    // `-1` as an option cluster, so `--workers` gets no argument at all. `--workers -1` therefore
    // reports a missing count rather than an invalid one. The code accepts this rather than working
    // around it, because claiming `-1` as this option's argument would mean second-guessing the
    // vendored parser's own split between options and arguments.
    record_error(options, "option '%s' requires a worker count", copt_curopt(opt));
  } else if (parse_size(value, &count) != 0 || count == 0) {
    // The value is raw `argv`, bounded only by `ARG_MAX`, so it trails the reason. A leading value
    // would make this the one value-carrying CLI message whose reason could truncate away.
    record_error(options, "option '--workers' must be a positive integer: '%s'", value);
  } else if (count > (size_t)WORKER_COUNT_MAX) {
    // The message prints both values as parsed numbers rather than as the raw argument, because a
    // `size_t` is bounded at twenty digits and so cannot truncate the limit off the end. This
    // matches `config key 'video_frame_seconds' exceeds max video frame seconds (...) at ...`,
    // the closest sibling.
    record_error(options, "option '--workers' exceeds max worker count (%zu) at %zu",
                 (size_t)WORKER_COUNT_MAX, count);
  } else {
    options->worker_count = count;
    // The code records `CLI_FLAG_WORKERS` as seen only on success, unlike `CLI_FLAG_VERBOSE`, which
    // it sets the moment it recognizes the flag. The asymmetry lets `config --workers 4` reach the
    // unsupported-flag diagnostic. A rejected value never needs that diagnostic, because
    // `record_error` keeps the first message and the value error above is already recorded. Nothing
    // depends on this placement. It is written this way so a recorded flag always means one that
    // parsed.
    *flags_seen |= CLI_FLAG_WORKERS;
  }
}

static void cli_parse_command(struct CliOptions* options,
                              int argc,
                              char** argv,
                              int positional_index,
                              unsigned flags_seen) {
  if (positional_index < argc) {
    for (size_t i = 0; i < sizeof(cli_commands) / sizeof(cli_commands[0]); i++) {
      if (cli_commands[i].name != NULL &&
          strcmp(argv[positional_index], cli_commands[i].name) == 0) {
        options->command = (enum CliCommand)i;
        break;
      }
    }
    if (options->command == CLI_COMMAND_NONE) {
      record_error(options, "unknown command '%s'", argv[positional_index]);
    } else {
      if (argc - positional_index > 1) {
        record_error(options, "unexpected argument '%s'", argv[positional_index + 1]);
      }
      reject_unsupported_flags(options, flags_seen);
    }
  }
}

static void reject_unsupported_flags(struct CliOptions* options, unsigned flags_seen) {
  const unsigned flags_unsupported = flags_seen & ~cli_commands[options->command].flags_accepted;
  for (size_t i = 0; i < sizeof(cli_flags) / sizeof(cli_flags[0]); i++) {
    if ((flags_unsupported & cli_flags[i].bit) != 0) {
      record_error(options, "command '%s' does not accept option '%s'",
                   cli_commands[options->command].name, cli_flags[i].name);
      return;
    }
  }
}

static int flush_stream(FILE* stream) {
  if (fflush(stream) != 0) {
    return -1;
  }
  if (ferror(stream) != 0) {
    errno = EIO;
    return -1;
  }
  return 0;
}
