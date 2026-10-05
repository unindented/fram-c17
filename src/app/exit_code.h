#ifndef FRAM_EXIT_CODE_H
#define FRAM_EXIT_CODE_H

/** Process exit codes returned by `main`, `cli_dispatch`, and the `cmd_*_run` entry points. */
enum ExitCode {
  /** The command completed successfully. */
  EXIT_CODE_OK = 0,

  /** The command ran but failed at runtime. */
  EXIT_CODE_FAILURE = 1,

  /** The command line could not be parsed. */
  EXIT_CODE_USAGE = 2,
};

// The values are interface: a shell or CI wrapper branches on them, and `2` for a usage error is
// the convention every option parser sets.
_Static_assert(EXIT_CODE_OK == 0 && EXIT_CODE_FAILURE == 1 && EXIT_CODE_USAGE == 2,
               "exit codes must keep their documented values");

#endif
