#ifndef FRAM_OS_MEMORY_H
#define FRAM_OS_MEMORY_H

#include <stdbool.h>
#include <stddef.h>

#include "core/path_list.h"
#include "shared/str.h"

struct Os;

// `os_memory.c` is the in-memory implementation of `src/runtime/os.h`. A unit test linking it
// builds its fixture tree with the functions below and passes the host to the code under test,
// which then reads, walks, stats and writes that tree as it would the disk. Nothing touches the
// filesystem, so a test needs no temporary directory, no cleanup, and no change of working
// directory.
//
// The tree models what the build can observe of a POSIX filesystem: directories, regular files,
// hard links (two entries naming one file, so one identity), and symlinks with relative or absolute
// targets, followed on every path component and reported as `ELOOP` past as many expansions as the
// kernel follows, 32 on macOS and 40 on Linux. A relative path and an absolute one name the same
// tree, whose root is also the working directory. Failures the tree cannot produce by its shape,
// such as `EACCES`, are injected with `os_memory_fail`.
//
// A path ending in `/` must name a directory, and a listing comes newest entry first, an order
// unlike the one a test builds its tree in. Each operation the code under test calls produces the
// message the POSIX implementation would for the same situation, so a test written against this
// host asserts the text a user sees. `src/runtime/test_os.c` runs the same tests against both hosts
// to keep them in agreement: a property of the tree that a test relies on here needs a test there.
// An injected failure is not one of those properties. It is reported with the message the POSIX
// implementation gives for that `errno` value, and it matches every path below the one it names,
// which permission bits on a real tree do not always reproduce. Every operation holds the host's
// lock, so the parallel phases can share one host.
//
// Modification times come from a counter that every change advances, so a later write is newer, and
// `os_memory_touch` makes a file newer without writing it. Modes are not modelled: what a write
// does to them is checked against the POSIX host alone, in `src/runtime/test_fs.c`.

/** Which operation an injected failure applies to. */
enum OsMemoryOp {
  /** `os_read` of the path, and its open as the source of an `os_write`. */
  OS_MEMORY_OP_READ,

  /** `os_write` of the path, as the open of the file itself fails. */
  OS_MEMORY_OP_WRITE,

  /**
   * `os_write` of the path, as the write of its bytes fails after the open, the way a full disk or
   * a file size limit fails it, at its first byte.
   */
  OS_MEMORY_OP_WRITE_DATA,

  /** `os_list` of the path, as its open fails. */
  OS_MEMORY_OP_LIST,

  /** `os_stat` of the path, and its inspection as an entry of a listing. */
  OS_MEMORY_OP_STAT,

  /** `os_remove` or `os_remove_dir` of the path. */
  OS_MEMORY_OP_REMOVE,
};

/**
 * @brief Returns a new host holding an empty root directory.
 *
 * The host lives in a reservation of its own, which acutest's child process returns when the test
 * ends.
 *
 * @return The host, never `NULL`.
 */
struct Os* os_memory(void) __attribute__((returns_nonnull));

/**
 * @brief Writes a file, creating its parent directories, as a fixture.
 *
 * Records a test failure when the path cannot be written, such as one below an existing file.
 *
 * @param os        Host to write to. Must not be `NULL`.
 * @param file_path Terminated path of the file. Must not be `NULL`.
 * @param contents  Terminated file contents. Must not be `NULL`.
 */
void os_memory_write(struct Os* os, const char* file_path, const char* contents)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Writes a file whose contents may hold any byte, creating its parent directories.
 *
 * @param os        Host to write to. Must not be `NULL`.
 * @param file_path Path of the file.
 * @param contents  File contents.
 */
void os_memory_write_bytes(struct Os* os, struct Str file_path, struct Str contents)
    __attribute__((nonnull(1)));

/**
 * @brief Extends a regular file to `len` bytes, as `truncate` does, as a fixture.
 *
 * The bytes past the old end read as `NUL`. The host stores only their count, so a test can model a
 * source far larger than its memory, as a sparse file does on disk.
 *
 * @param os        Host holding the file. Must not be `NULL`.
 * @param file_path Terminated path of an existing regular file. Must not be `NULL`.
 * @param len       New size in bytes. Must not be below the file's size.
 */
void os_memory_truncate(struct Os* os, const char* file_path, ptrdiff_t len)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Makes the file at `file_path` newer than every file written before, as `touch` does.
 *
 * @param os        Host holding the file. Must not be `NULL`.
 * @param file_path Terminated path of an existing file, followed through symlinks. Must not be
 *                  `NULL`.
 */
void os_memory_touch(struct Os* os, const char* file_path) __attribute__((nonnull(1, 2)));

/**
 * @brief Creates a directory and any missing parents, as a fixture.
 *
 * @param os       Host to create the directory on. Must not be `NULL`.
 * @param dir_path Terminated path of the directory. Must not be `NULL`.
 */
void os_memory_mkdir(struct Os* os, const char* dir_path) __attribute__((nonnull(1, 2)));

/**
 * @brief Creates a symlink at `link_path` whose target is `target`, as `symlink` does.
 *
 * The target need not exist. A relative target resolves from the directory holding the link.
 * Missing parents of `link_path` are created.
 *
 * @param os        Host to create the link on. Must not be `NULL`.
 * @param target    Terminated target text. Must not be `NULL`.
 * @param link_path Terminated path of the new link. Must not be `NULL`.
 */
void os_memory_symlink(struct Os* os, const char* target, const char* link_path)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Creates a hard link at `link_path` to the existing file `file_path`, as `link` does.
 *
 * @param os        Host to create the link on. Must not be `NULL`.
 * @param file_path Terminated path of an existing regular file. Must not be `NULL`.
 * @param link_path Terminated path of the new entry. Must not be `NULL`.
 */
void os_memory_link(struct Os* os, const char* file_path, const char* link_path)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Makes every later `op` on `path`, or on a path below it, fail with `error_number`.
 *
 * The match is on the path text the code under test passes, not on what it resolves to, so a test
 * names the spelling the code uses. An `error_number` of `0` lifts the failures injected for `op`
 * on that same `path` text, so a test can run the code again once the cause is gone.
 *
 * @param os           Host to inject into. Must not be `NULL`.
 * @param op           Operation that fails.
 * @param path         Terminated path the failure applies to, without a trailing `/`, which fails
 *                     the test. Must not be `NULL`.
 * @param error_number `errno` value the operation fails with.
 */
void os_memory_fail(struct Os* os, enum OsMemoryOp op, const char* path, int error_number)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Reports whether `file_path` names a regular file holding exactly `contents`.
 *
 * Records what the file holds, or that it is missing, when it does not match.
 *
 * @param os        Host to inspect. Must not be `NULL`.
 * @param file_path Terminated path of the file, followed through symlinks. Must not be `NULL`.
 * @param contents  Terminated contents the file must hold. Must not be `NULL`.
 * @return `true` when the file exists and holds `contents`.
 */
bool os_memory_is_file(struct Os* os, const char* file_path, const char* contents)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Reports whether anything exists at `path`, without following a symlink at its end.
 *
 * @param os   Host to inspect. Must not be `NULL`.
 * @param path Terminated path to look up. Must not be `NULL`.
 * @return `true` when a file, directory or symlink exists at `path`.
 */
bool os_memory_has_path(struct Os* os, const char* path) __attribute__((nonnull(1, 2)));

/**
 * @brief Reads the contents of the regular file at `file_path`, such as an encoded image a test
 *        decodes.
 *
 * @param os        Host to inspect. Must not be `NULL`.
 * @param file_path Terminated path of the file, followed through symlinks. Must not be `NULL`.
 * @param data_out  Receives the contents, which the host owns. Written only on success. Must not be
 *                  `NULL`.
 * @return `0` on success, or `-1` when there is no regular file at `file_path`, or only one that
 *         `os_memory_truncate` extended.
 */
int os_memory_file(struct Os* os, const char* file_path, struct Str* data_out)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Lists every regular file below `dir_path`, sorted, without following symlinks.
 *
 * @param os       Host to inspect. Must not be `NULL`.
 * @param dir_path Terminated path of the directory to list. Must not be `NULL`.
 * @return The paths, each `dir_path` joined with the path below it, in a list the host owns. Empty
 *         when `dir_path` is not a directory.
 */
struct PathList os_memory_files(struct Os* os, const char* dir_path) __attribute__((nonnull(1, 2)));

/**
 * @brief Returns everything `os_print` has written to `fd` so far.
 *
 * @param os Host to inspect. Must not be `NULL`.
 * @param fd `1` for standard output or `2` for standard error.
 * @return The text, which the host owns.
 */
struct Str os_memory_printed(struct Os* os, int fd) __attribute__((nonnull(1)));

/**
 * @brief Reports whether `os_print` has written exactly `expected` to `fd` so far.
 *
 * Records what the stream holds when it does not match.
 *
 * @param os       Host to inspect. Must not be `NULL`.
 * @param fd       `1` for standard output or `2` for standard error.
 * @param expected Terminated text the stream must hold. Must not be `NULL`.
 * @return `true` when the stream holds exactly `expected`.
 */
bool os_memory_is_printed(struct Os* os, int fd, const char* expected)
    __attribute__((nonnull(1, 3)));

#endif
