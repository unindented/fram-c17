#ifndef FRAM_CMD_CONFIG_H
#define FRAM_CMD_CONFIG_H

#include "app/exit_code.h"

/**
 * @brief Runs the `config` command: loads `fram.toml` and prints the resolved configuration.
 *
 * On success prints the effective configuration to `stdout`. On failure prints the reason to
 * `stderr`, whether `fram.toml` could not be loaded or `stdout` could not be written.
 *
 * @return `EXIT_CODE_OK` on success, or `EXIT_CODE_FAILURE` when `fram.toml` cannot be loaded or
 *         the resolved configuration cannot be written to `stdout`.
 */
enum ExitCode cmd_config_run(void);

#endif
