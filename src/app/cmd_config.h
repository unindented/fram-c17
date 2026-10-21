#ifndef FRAM_CMD_CONFIG_H
#define FRAM_CMD_CONFIG_H

#include "app/exit_code.h"

struct Os;

/**
 * @brief Runs the `config` command: loads `fram.toml` and prints the resolved configuration.
 *
 * On success prints the effective configuration to `stdout`. On a load failure prints the reason to
 * `stderr`. A failed write to `stdout` prints nothing.
 *
 * @param os Host the configuration is read from and printed on. Must not be `NULL`.
 * @return `EXIT_CODE_OK` on success, or `EXIT_CODE_FAILURE` when `fram.toml` cannot be loaded or
 *         the resolved configuration cannot be written to `stdout`.
 */
enum ExitCode cmd_config_run(struct Os* os) __attribute__((nonnull(1)));

#endif
