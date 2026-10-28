#ifndef FRAM_OS_H
#define FRAM_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shared/arena.h"
#include "shared/str.h"

struct Log;

// Every operation the build performs on the host goes through the functions below, and nothing else
// in `src/` makes a system call on a path, except the video code below. Each is declared once here
// and defined by exactly one implementation per executable, chosen when it is linked, as `_ssg`
// does with `main_posix.cpp` and `main_test.cpp`:
//
// - `runtime/fs.c` is the POSIX implementation, which the `fram` executable and every unit test
//   registered without `OS memory` link (`cmake/fram_testing.cmake` says which tests those are and
//   why).
// - `tests/os_memory.c` is an in-memory implementation, which the unit tests registered with
//   `OS memory` link, so they build their trees without touching the disk.
//
// A link-time choice needs no table of function pointers, and the build never needs two
// implementations in one process. `struct Os` is defined by the implementation, so each carries its
// own state: none for POSIX, the whole tree for the in-memory one.
//
// `os_oom`, the remaining member of the interface, is declared in `shared/arena.h`, because the
// arena calls it. It is not defined here but by each program: `app/main.c` for the executable and
// `tests/test_support.c` for every unit test, whichever implementation it links.
//
// Running other programs is not part of the interface. `runtime/proc.c` and `runtime/video.c` spawn
// `ffprobe` and `ffmpeg` on the real host whichever implementation an executable links, and
// `video.c` creates, inspects and removes the temporary frame `ffmpeg` writes, a file on that host
// that only an executable linking the POSIX implementation reads back. A fake would have to emulate
// what `ffmpeg` writes, which tests `ffmpeg` rather than the host. An executable on the in-memory
// host never sees a video, because the build checks for the tools only when a video is present.
//
// Every path is a `struct Str`. A path the POSIX system could not take whole fails rather than
// naming some other file: one of `PATH_MAX` bytes or more fails as `ENAMETOOLONG`, as the system
// would fail it, and one holding a `NUL` byte fails as `EINVAL`, because the system would read only
// the bytes before it. A path that ends in `/` must name a directory, so a read, a listing or a
// `stat` of one that names anything else fails as `ENOTDIR`, even on a system that would resolve
// `link/` to the file `link` points to.
//
// Every operation below that can fail and takes a `log` also takes the caller's `operation`, such
// as `failed to read config`, and records one error in `log` that leads with it and then gives the
// cause, as in `failed to read config: No such file or directory ('fram.toml')`. The caller owns
// the operation and its attribution, because one failed `os_write` is
// `failed to write template output` from the aggregate write and names its album from the album
// page write, while only the implementation knows which path failed: the file itself, or a
// directory partway through a `mkdir -p`. First-party causes are lowercase. Operating-system
// messages keep their spelling.

/** The host the build runs against. Each implementation defines it. */
struct Os;

/**
 * Identity of one existing file, as the device and inode pair naming it uniquely.
 *
 * Comparing identities answers "are these two paths the same file?" where comparing path strings
 * cannot. `./photos/x.jpg` and `photos/x.jpg` are the same file spelled two ways, and so are a
 * symlink and its target, a hard link and its twin, and two spellings that differ only in case on a
 * case-insensitive filesystem. These fields are wider than `dev_t` and `ino_t` so this header does
 * not put system types on its callers.
 */
struct OsIdentity {
  /** Device the file resides on. */
  uint64_t device;

  /** Inode number within `device`. */
  uint64_t inode;
};

/**
 * @brief Orders two identities by device and then inode, for `qsort`, `bsearch` and a sorted set.
 *
 * It is header-only and `static inline`, because its body is two comparisons and every caller of
 * `os_stat` that compares identities needs the same order.
 *
 * @param a First `struct OsIdentity`. Must not be `NULL`.
 * @param b Second `struct OsIdentity`. Must not be `NULL`.
 * @return A negative value, zero, or a positive value as `a` orders before, equal to, or after `b`.
 */
static inline int os_identity_compare(const void* a, const void* b) {
  const struct OsIdentity* left = a;
  const struct OsIdentity* right = b;
  if (left->device != right->device) {
    return left->device < right->device ? -1 : 1;
  }
  if (left->inode != right->inode) {
    return left->inode < right->inode ? -1 : 1;
  }
  return 0;
}

/** What `os_stat` reports about an existing path. */
struct OsStat {
  /** Identity of the file the path names, after following symlinks. */
  struct OsIdentity identity;

  /** Whether the path names a directory, after following symlinks. */
  bool is_dir;

  /**
   * Size in bytes of the file the path names, saturating at `PTRDIFF_MAX`. A freshness check treats
   * an empty output as stale, and a copied original must also match its source's size.
   */
  ptrdiff_t len;

  /** Modification time in whole seconds since the Unix epoch. */
  int64_t mtime_s;

  /**
   * Nanoseconds within `mtime_s`. A freshness check compares an output against its source at this
   * resolution because a build regenerates an output in the same second its source changed, and a
   * whole-second comparison would then accept a stale output as current.
   */
  int64_t mtime_ns;
};

/** What a directory entry names, as `os_list` classifies it. */
enum OsEntryKind {
  /** A regular file, or a symlink that resolves to one. */
  OS_ENTRY_FILE,

  /** A directory that is not a symlink. */
  OS_ENTRY_DIR,

  /** A symlink that resolves to a directory. */
  OS_ENTRY_DIR_LINK,

  /**
   * An entry that exists but could not be inspected, such as one below a directory that is readable
   * but not searchable.
   */
  OS_ENTRY_FAILED,
};

/** One entry of a directory listing. */
struct OsEntry {
  /** Bare entry name, never `.` or `..`. */
  struct Str name;

  /** What the entry names. */
  enum OsEntryKind kind;

  /** `errno` value of the failed inspection of an `OS_ENTRY_FAILED` entry, or `0`. */
  int error_number;
};

/**
 * @brief Reads a regular file into `perm`.
 *
 * It writes the output parameter only on success, and a failed read leaves `perm` as it found it.
 * It rejects a file that is not regular, one larger than `data_len_max`, and one that changes size
 * during the read. It never reports a partial copy as a successful read. A FIFO is rejected rather
 * than waited on. The size limit is checked before any byte is read or allocated, so an oversize
 * input never becomes resident. Each caller passes the limit for the kind of file it reads.
 *
 * The bytes may contain arbitrary data including `NUL`, because this also reads binary media. Text
 * consumers reject a `NUL` themselves by calling `text_is_nul_free`.
 *
 * @param os           Host to read from. Must not be `NULL`.
 * @param file_path    Path of the file to read.
 * @param data_len_max Largest accepted file size in bytes.
 * @param perm         Arena that owns the file bytes. Must not be `NULL`.
 * @param data_out     Receives the file bytes. Must not be `NULL`.
 * @param operation    Operation leading the failure message, such as `failed to read config`. The
 *                     message names `file_path`, and its cause tells the first-party rejections
 *                     apart from the system ones.
 * @param log          Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file is missing, not regular, larger than `data_len_max`
 *         or changed size mid-read, and on an open, read or close failure.
 */
int os_read(struct Os* os,
            struct Str file_path,
            ptrdiff_t data_len_max,
            struct Arena* perm,
            struct Str* data_out,
            struct Str operation,
            struct Log* log) __attribute__((nonnull(1, 4, 5, 7)));

/** Where the bytes `os_write` writes come from. */
enum OsSourceKind {
  /**
   * `OsSource.bytes`, whole. The zero kind, so `(struct OsSource){.bytes = data}` writes `data`.
   */
  OS_SOURCE_BYTES,

  /**
   * The regular file at `OsSource.file_path`, streamed in bounded chunks, so a file of any size is
   * copied without being read whole into memory. A new destination does not take the source's mode,
   * so a published copy's permissions do not depend on the source tree, and a `0600` stylesheet
   * does not publish unreadable.
   */
  OS_SOURCE_FILE,
};

/** The bytes one `os_write` writes. Only the fields of its kind are read. */
struct OsSource {
  /** Which of the fields below holds the bytes. */
  enum OsSourceKind kind;

  /** Bytes to write, for `OS_SOURCE_BYTES`. */
  struct Str bytes;

  /** Regular file to copy, for `OS_SOURCE_FILE`. */
  struct Str file_path;
};

/**
 * @brief Writes the bytes of `source` to `file_path`, creating parent directories as needed.
 *
 * It overwrites any existing file in place. That write is deliberately neither atomic nor
 * `fsync`ed, so a reader can observe a partial file while a write is running and an interrupted
 * build can leave one behind. That is acceptable because an `output_dir` is reproducible: deleting
 * it and building again recovers a truncated file in one command, and recovers stale outputs and a
 * half-finished phase at the same time, which a temp-write-plus-rename protocol would not. A newly
 * created file gets mode `0666` reduced by the process umask. An existing destination keeps its own
 * mode, because the write goes through the file already there, and so does every other name of it:
 * a symlink is followed, and a hard link sees the new bytes. The destination's directory needs no
 * write permission when the file already exists.
 *
 * This, `os_mkdirs`, `os_remove` and `os_remove_dir` are the only operations that change the host,
 * and should stay the only ones. A second writer covering a subset of the outputs gives a caller a
 * choice to get wrong, which is why both sources share this one function, its open, its parents and
 * its messages. Parallel jobs call it at once, for distinct files that may share parent
 * directories.
 *
 * @param os        Host to write to. Must not be `NULL`.
 * @param file_path Destination path.
 * @param source    Bytes to write.
 * @param operation Operation leading the failure message. The message names `file_path`, the parent
 *                  directory component that could not be created, or a source file that could not
 *                  be read.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a directory, open, read, write or close failure, or a source
 *         file that is missing or not regular.
 */
int os_write(struct Os* os,
             struct Str file_path,
             struct OsSource source,
             struct Str operation,
             struct Log* log) __attribute__((nonnull(1, 5)));

/**
 * @brief Creates `dir_path` and any missing parent directories.
 *
 * It accepts existing directories along the path rather than treating them as errors, so two
 * callers creating one directory at once both succeed.
 *
 * @param os        Host to create the directories on. Must not be `NULL`.
 * @param dir_path  Directory path to create. An empty string is a no-op.
 * @param operation Operation leading the failure message, which names the component that could not
 *                  be created.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` if a component cannot be created.
 */
int os_mkdirs(struct Os* os, struct Str dir_path, struct Str operation, struct Log* log)
    __attribute__((nonnull(1, 4)));

/**
 * @brief Removes the file or symlink `file_path` names, as `unlink` does.
 *
 * A symlink is removed itself, never what it points to, and a file with another name keeps its
 * bytes under that name. A path that names nothing counts as removed, so a caller that removes what
 * it listed does not fail on an entry something else removed first.
 *
 * @param os        Host to remove from. Must not be `NULL`.
 * @param file_path Path of the file or symlink to remove.
 * @param operation Operation leading the failure message, which names `file_path`.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success or when `file_path` names nothing, or `-1` when it names a directory or
 *         cannot be removed.
 */
int os_remove(struct Os* os, struct Str file_path, struct Str operation, struct Log* log)
    __attribute__((nonnull(1, 4)));

/**
 * @brief Removes the empty directory `dir_path` names, as `rmdir` does.
 *
 * A path that names nothing counts as removed, as it does for `os_remove`.
 *
 * @param os        Host to remove from. Must not be `NULL`.
 * @param dir_path  Path of the directory to remove. A symlink to a directory is not one.
 * @param operation Operation leading the failure message, which names `dir_path`.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success or when `dir_path` names nothing, or `-1` when it names something other
 *         than a directory, a directory that is not empty, or one that cannot be removed.
 */
int os_remove_dir(struct Os* os, struct Str dir_path, struct Str operation, struct Log* log)
    __attribute__((nonnull(1, 4)));

/**
 * @brief Lists the entries of one directory that name a regular file or a directory.
 *
 * Leaves out `.` and `..`, and silently skips an entry that is neither a regular file nor a
 * directory once symlinks are followed, and one that resolves to nothing: a dangling symlink, one
 * removed since the directory was read, or a symlink that resolves in a cycle. No file exists
 * behind those, so a caller loses nothing by never seeing them. An entry that exists but cannot be
 * inspected is listed as `OS_ENTRY_FAILED` with its `errno`, so a file the caller could not look at
 * is never silently missing, and the caller reports it where its own order reaches it. So is a
 * symlink through a regular file, such as `x -> photo.jpg/y`, which fails as `ENOTDIR`. The entries
 * come in no particular order.
 *
 * The directory is closed again before this returns, so a recursive walk built on it holds no
 * directory open while it descends, and the depth of a tree is bounded by path length rather than
 * by the open-file limit.
 *
 * @param os          Host to list. Must not be `NULL`.
 * @param dir_path    Directory to list.
 * @param perm        Arena that owns the entry array and the names in it. Must not be `NULL`.
 * @param entries_out Receives the entry array. Written only on success. Must not be `NULL`.
 * @param count_out   Receives the number of entries. Written only on success. Must not be `NULL`.
 * @param operation   Operation leading the failure message, such as `failed to list source media`.
 *                    The message then names the step that failed and the directory, as in
 *                    `cannot open directory: Permission denied ('x')`.
 * @param log         Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on an open, read or close failure.
 */
int os_list(struct Os* os,
            struct Str dir_path,
            struct Arena* perm,
            struct OsEntry** entries_out,
            ptrdiff_t* count_out,
            struct Str operation,
            struct Log* log) __attribute__((nonnull(1, 3, 4, 5, 7)));

/**
 * @brief Reports the identity, type, size and modification time of the file at `file_path`,
 *        following symlinks.
 *
 * This takes no `log`, because its callers want a fact rather than a diagnostic. A path that does
 * not exist has no identity and is not a failure worth reporting: it is the ordinary case for an
 * output path about to be created. A caller that does need to say why reads `errno`.
 *
 * @param os        Host to inspect. Must not be `NULL`.
 * @param file_path Path to inspect.
 * @param stat_out  Receives what the path names. Written only on success. Must not be `NULL`.
 * @return `0` when `stat_out` was filled, or `-1` with `errno` set when `file_path` could not be
 *         inspected.
 */
int os_stat(struct Os* os, struct Str file_path, struct OsStat* stat_out)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Writes `text` to standard output (`1`) or standard error (`2`).
 *
 * Every byte the program prints goes through here: the log a command ends with, a rejected command
 * line, `--help` and `--version`, the resolved configuration, the verbose status lines and the
 * workers' progress line, so a test on the in-memory host can capture all of it. Nothing in `src/`
 * prints through `stdio`, whose buffer `_Exit` would drop.
 *
 * One call writes its text through one descriptor, retrying a short or interrupted write, and
 * nothing else in the program buffers it. Two threads printing at once can still interleave, so a
 * caller that prints from several threads, as the workers' progress line does, holds its own lock
 * around a call that carries the whole line.
 *
 * @param os   Host to print on. Must not be `NULL`.
 * @param fd   `1` for standard output or `2` for standard error.
 * @param text Bytes to write.
 * @return `0` once every byte is written, or `-1` with `errno` set on a write failure.
 */
int os_print(struct Os* os, int fd, struct Str text) __attribute__((nonnull(1)));

#endif
