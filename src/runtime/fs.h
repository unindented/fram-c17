#ifndef FRAM_FS_H
#define FRAM_FS_H

struct Os;

// `fs.c` is the POSIX implementation of every operation `runtime/os.h` declares. This header adds
// only the way to get one.

/**
 * @brief Returns the POSIX host.
 *
 * It carries no state, so every call returns the same host and nothing has to release it.
 *
 * @return The host, never `NULL`.
 */
struct Os* os_posix(void) __attribute__((returns_nonnull));

#endif
