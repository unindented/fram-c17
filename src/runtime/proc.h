#ifndef FRAM_PROC_H
#define FRAM_PROC_H

#include <stddef.h>

/**
 * @brief Runs one program without a shell and captures its standard output.
 *
 * `argv` is always an argument array, never a shell command. Spaces, quotes, and metacharacters in
 * a media path therefore remain literal bytes rather than becoming command injection. Standard
 * output and standard error are drained together with `poll`, preventing either pipe from blocking
 * the child while the parent reads the other. On a failed exit, `reason` ends with the newest
 * standard-error bytes that fit, marked with a leading `...` when older bytes were dropped, because
 * a child's last line is usually the one that names the cause.
 *
 * @param argv       Null-terminated argument vector whose first entry is the program. Must not be
 *                   `NULL` and must contain at least one entry.
 * @param output     Receives terminated standard output when `output_len` is nonzero. May be `NULL`
 *                   only when `output_len` is zero.
 * @param output_len Size of `output`, including its terminator.
 * @param reason     Receives a failure reason. May be `NULL` only when `reason_len` is zero.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` when the child exits with status zero and output fits, or `-1` otherwise.
 */
int proc_run(const char* const* argv,
             char* output,
             size_t output_len,
             char* reason,
             size_t reason_len) __attribute__((nonnull(1)));

#endif
