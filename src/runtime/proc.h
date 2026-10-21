#ifndef FRAM_PROC_H
#define FRAM_PROC_H

#include "shared/str.h"

struct Arena;
struct Log;

/**
 * Most standard output `proc_run` keeps from one child, in bytes. Only `ffprobe`'s duration is
 * captured, one short line, so a child that prints more is not printing a duration.
 */
enum { PROC_OUTPUT_LEN_MAX = 64 * 1024 };

/**
 * @brief Runs one program without a shell and captures its standard output.
 *
 * `argv` is always an argument array, never a shell command. Spaces, quotes, and metacharacters in
 * a media path therefore remain literal bytes rather than becoming command injection. Standard
 * output and standard error are drained together with `poll`, preventing either pipe from blocking
 * the child while the parent reads the other. On a failed exit, the message ends with the newest
 * standard-error bytes the tail kept, marked with a leading `...` when older bytes were dropped,
 * because a child's last line is usually the one that names the cause. The tail is shown on one
 * line, each run of line breaks in it as `; `.
 *
 * The child gets `/dev/null` as standard input, the two pipes as its standard output and standard
 * error, and no other descriptor this program opened. Every descriptor the program opens is
 * close-on-exec, so a child that one worker spawns never holds another worker's pipe or file, and
 * each worker sees the end of its own child's output when that child exits.
 *
 * @param argv       Null-terminated argument vector whose first entry is the program. Must not be
 *                   `NULL` and must contain at least one entry.
 * @param arena      Arena for the copy of `argv` the spawn takes and for the captured output. Must
 *                   not be `NULL`.
 * @param output_out Receives standard output, owned by `arena`, on success, so a `NUL` the child
 *                   printed stays part of it. `NULL` discards standard output.
 * @param operation  Operation leading the failure message, such as `failed to probe media`.
 * @param file_path  File the child works on, named at the end of the message, or empty to name
 *                   none.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` when the child exits with status zero and prints at most `PROC_OUTPUT_LEN_MAX` bytes
 *         of standard output, or `-1` otherwise.
 */
int proc_run(const char* const* argv,
             struct Arena* arena,
             struct Str* output_out,
             struct Str operation,
             struct Str file_path,
             struct Log* log) __attribute__((nonnull(1, 2, 6)));

#endif
