#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "runtime/fs.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/text.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"

/**
 * The POSIX host. It has no state of its own: the process's working directory, umask and file
 * descriptors are the state, and every call reaches them through the system.
 */
struct Os {
  /** Unused. ISO C does not allow a structure without members. */
  char unused;
};

/**
 * Where an action reports its failure: the caller's log, and the caller's operation, which leads
 * every message so the user reads what failed before why, as in
 * `failed to read config: No such file or directory ('fram.toml')`.
 */
struct FsReport {
  /** Operation the caller was performing, such as `failed to read config`. */
  struct Str operation;

  /** Log that receives the message. */
  struct Log* log;
};

/** Bytes `os_write` moves per `read` and `write` when it streams a source file. */
enum { FS_WRITE_BUFFER_SIZE = 64 * 1024 };

/** The file one `os_write` call writes, from its open to its close. */
struct FsWriteTarget {
  /** Destination path, named in a failure. */
  struct Str file_path;

  /** Descriptor open for writing, or `-1` before the open succeeds. */
  int fd;

  /** Terminated destination path. */
  char path[PATH_MAX];
};

/** What `os_list_classify` found one directory entry to be. */
enum ListClassify {
  /** A regular file or a directory, once symlinks are followed, so the listing keeps it. */
  LIST_CLASSIFY_LISTED,

  /** Another kind of file, or a symlink that resolves to nothing, so the listing leaves it out. */
  LIST_CLASSIFY_SKIPPED,

  /** An entry that exists but could not be inspected, with `errno` set, so the listing fails. */
  LIST_CLASSIFY_FAILED,
};

/**
 * @brief Reads the whole of an open file into `data` and closes it. A step of `os_read`.
 *
 * Reads through a stream over `fd` and closes the stream whatever happens, so a close error fails
 * the read too. Only the first failure is reported, because a later one is a consequence of it.
 *
 * @param data      Buffer of at least `data_len` bytes that receives the file contents. Must not be
 *                  `NULL`.
 * @param data_len  Number of bytes to read, taken from the `fstat` of the open file. Must not be
 *                  negative.
 * @param fd        Descriptor positioned at the start of the file, which this closes.
 * @param file_path Path of the file, named in a failure.
 * @param report    Where a failure is reported. Must not be `NULL`.
 * @return `0` when exactly `data_len` stable bytes were read and the file closed, or `-1`
 *         otherwise.
 */
static int os_read_bytes(char* data,
                         ptrdiff_t data_len,
                         int fd,
                         struct Str file_path,
                         const struct FsReport* report) __attribute__((nonnull(1, 5)));

/**
 * @brief Reads exactly `data_len` bytes from a stream, verifying the file did not change
 *        underneath. A step of `os_read_bytes`.
 *
 * `fstat` and `fread` are two observations of a file another process may be writing, so a read that
 * returns anything other than `data_len` bytes is a changed file rather than a partial success.
 * Both directions are rejected: too few bytes means the file shrank, and one readable byte past
 * `data_len` means it grew. Byte values have no policy here; text consumers validate separately.
 *
 * @param data      Buffer of at least `data_len` bytes that receives the file contents. Must not be
 *                  `NULL`.
 * @param data_len  Number of bytes to read. Must not be negative.
 * @param fp        Stream positioned at the start of the file. Must not be `NULL`.
 * @param file_path Path of the file, named in a failure.
 * @param report    Where a failure is reported. Must not be `NULL`.
 * @return `0` when exactly `data_len` stable bytes were read, or `-1` otherwise.
 */
static int os_read_bytes_exact(char* data,
                               ptrdiff_t data_len,
                               FILE* fp,
                               struct Str file_path,
                               const struct FsReport* report) __attribute__((nonnull(1, 3, 5)));

/**
 * @brief Opens the destination of an `os_write` call, creating or truncating it. A step of
 *        `os_write`.
 *
 * @param target Target whose `file_path` is set. Receives the open descriptor and the terminated
 *               path. Must not be `NULL`.
 * @param report Where a failure is reported. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file cannot be opened or created.
 */
static int os_write_open(struct FsWriteTarget* target, const struct FsReport* report)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes every byte of `source` to `fd`. A step of `os_write`.
 *
 * @param fd        Descriptor open for writing.
 * @param source    Bytes to write.
 * @param source_fd Descriptor of the open source file of an `OS_SOURCE_FILE` write, or `-1`.
 * @param file_path Destination path, named in a write failure.
 * @param report    Where a failure is reported. Must not be `NULL`.
 * @return `0` once every byte is written, or `-1` on a read or write failure.
 */
static int os_write_source(int fd,
                           struct OsSource source,
                           int source_fd,
                           struct Str file_path,
                           const struct FsReport* report) __attribute__((nonnull(5)));

/**
 * @brief Copies the open source file of an `OS_SOURCE_FILE` write to `fd`.
 *
 * @param fd          Descriptor open for writing.
 * @param source_fd   Descriptor of the source file, positioned at its start.
 * @param source_path Source path, named in a read failure.
 * @param file_path   Destination path, named in a write failure.
 * @param report      Where a failure is reported. Must not be `NULL`.
 * @return `0` once the whole source is copied, or `-1` on a read or write failure.
 */
static int os_write_source_stream(int fd,
                                  int source_fd,
                                  struct Str source_path,
                                  struct Str file_path,
                                  const struct FsReport* report) __attribute__((nonnull(5)));

/**
 * @brief Closes the descriptors an `os_write` call opened. A step of `os_write`.
 *
 * @param target      Target, open or not. Must not be `NULL`.
 * @param source_fd   Descriptor of the open source file of an `OS_SOURCE_FILE` write, or `-1`.
 * @param source_path Source path, named in a close failure.
 * @param rc          Result of the write so far. A failure in it is the one reported.
 * @param report      Where a failure is reported. Must not be `NULL`.
 * @return `0` when `rc` is `0` and every close succeeds, or `-1`.
 */
static int os_write_close(const struct FsWriteTarget* target,
                          int source_fd,
                          struct Str source_path,
                          int rc,
                          const struct FsReport* report) __attribute__((nonnull(1, 5)));

/**
 * @brief Creates `dir_path` and any missing parent directories. The body of `os_mkdirs`, which
 *        `os_write` shares for a file's parent directories so a write reports its own operation.
 *
 * @param dir_path Directory path to create. An empty string is a no-op.
 * @param report   Where a failure is reported, naming the component that could not be created. Must
 *                 not be `NULL`.
 * @return `0` on success, or `-1` if a component cannot be created.
 */
static int mkdirs_report(struct Str dir_path, const struct FsReport* report)
    __attribute__((nonnull(2)));

/**
 * @brief Creates one directory, accepting a path that already names a directory.
 *
 * Creates only the named directory. `mkdirs_report` walks the path and calls this per component.
 *
 * @param dir_path Directory path to create.
 * @param report   Where a failure is reported, naming `dir_path` since it is one component of a
 *                 path the caller named whole. Must not be `NULL`.
 * @return `0` on success or when `dir_path` already names a directory, or `-1` on failure.
 */
static int mkdirs_report_one(struct Str dir_path, const struct FsReport* report)
    __attribute__((nonnull(2)));

/**
 * @brief Reads every entry name in `dir_path` except `.` and `..`.
 *
 * @param names    Path list that receives the bare entry names. Must not be `NULL`.
 * @param dir_path Directory to read.
 * @param arena    Arena that owns `names` and the names in it. Must not be `NULL`.
 * @param report   Where a failure is reported, naming `dir_path`. Must not be `NULL`.
 * @return `0` on success, or `-1` on an open, read, or close failure.
 */
static int os_list_names(struct PathList* names,
                         struct Str dir_path,
                         struct Arena* arena,
                         const struct FsReport* report) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Classifies one directory entry, following it when it is a symlink.
 *
 * @param file_path Entry path to inspect.
 * @param kind_out  Receives what the entry names. Must not be `NULL`.
 * @return `LIST_CLASSIFY_LISTED` when the entry names a regular file or a directory and the outputs
 *         are written, `LIST_CLASSIFY_SKIPPED` for another kind of file or one that resolves to
 *         nothing, or `LIST_CLASSIFY_FAILED` with `errno` set when it exists but could not be
 *         inspected.
 */
static enum ListClassify os_list_classify(struct Str file_path, enum OsEntryKind* kind_out)
    __attribute__((nonnull(2)));

/**
 * @brief Opens a file to read from and checks that it is a regular file, for `os_read` and the
 *        source of an `OS_SOURCE_FILE` write.
 *
 * It opens without blocking and inspects the descriptor rather than the path, so the file checked
 * is the file read, and a path swapped to a FIFO cannot block the open waiting for a writer. The
 * flag is cleared again once the file is known to be regular.
 *
 * @param file_path File to open.
 * @param fd_out    Receives the open descriptor. Written only on success. Must not be `NULL`.
 * @param st_out    Receives the `fstat` of the descriptor. Must not be `NULL`.
 * @param report    Where a failure is reported, naming `file_path`. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file cannot be opened or inspected, or is not regular.
 */
static int open_regular(struct Str file_path,
                        int* fd_out,
                        struct stat* st_out,
                        const struct FsReport* report) __attribute__((nonnull(2, 3, 4)));

/**
 * Most bytes `write_all` passes to one `write`, because macOS refuses a count above `INT_MAX` with
 * `EINVAL`.
 */
enum { FS_WRITE_LEN_MAX = 1024 * 1024 * 1024 };

/**
 * @brief Writes every byte to an open descriptor, retrying interrupted and short writes.
 *
 * @param fd   Descriptor open for writing.
 * @param data Bytes to write.
 * @return `0` once every byte is written, or `-1` with `errno` set on a write error, or to `EIO` on
 *         a write that makes no progress, which no system error of its own describes.
 */
static int write_all(int fd, struct Str data);

/**
 * @brief Copies a path into a terminated buffer for a system call.
 *
 * @param path     Path to convert.
 * @param cstr_out Buffer of `PATH_MAX` bytes that receives the terminated path. Must not be `NULL`.
 * @return `0` on success, or `-1` with `errno` set to `ENAMETOOLONG` when the path and its
 *         terminator exceed `PATH_MAX`, or to `EINVAL` when it holds a `NUL` byte.
 */
static int copy_cstr(struct Str path, char cstr_out[static PATH_MAX]);

/**
 * @brief Reports whether a path ends in `/` but names something other than a directory.
 *
 * A path that ends in `/` must name a directory, and POSIX has `open` and `stat` fail with
 * `ENOTDIR` when it does not. Some systems resolve `link/`, where `link` is a symlink to a file, as
 * the file, so the host checks what the call found.
 *
 * @param path Path the caller gave.
 * @param mode `st_mode` of what it found.
 * @return `true` when `path` ends in `/` and `mode` is not a directory's, or `false` otherwise.
 */
static bool is_slash_on_non_dir(struct Str path, mode_t mode);

/**
 * @brief Reports `error_number` as the cause of the caller's operation failing on `path`.
 *
 * This is for a failure on the path the caller's operation targeted, as in
 * `failed to read config: No such file or directory ('fram.toml')`.
 *
 * @param report       Where the failure is reported. Must not be `NULL`.
 * @param path         Path the operation targeted.
 * @param error_number `errno` value to describe.
 * @return `-1` always, so a caller can write `return report_errno(...);`.
 */
static int report_errno(const struct FsReport* report, struct Str path, int error_number)
    __attribute__((nonnull(1)));

/**
 * @brief Reports `error_number` as the cause of one step of the caller's operation failing.
 *
 * This is for a failure on a path the caller cannot name, such as a directory entry or one
 * component of a path created whole, so the message names the step as well, as in
 * `failed to list static files: cannot open directory: <cause> ('<dir>')`.
 *
 * @param report       Where the failure is reported. Must not be `NULL`.
 * @param step         Lowercase step that failed, such as `open directory`.
 * @param path         Path the step was attempted on.
 * @param error_number `errno` value to describe.
 * @return `-1` always, so a caller can write `return report_step_errno(...);`.
 */
static int report_step_errno(const struct FsReport* report,
                             struct Str step,
                             struct Str path,
                             int error_number) __attribute__((nonnull(1)));

struct Os* os_posix(void) {
  static struct Os os;
  return &os;
}

int os_read(struct Os* os,
            struct Str file_path,
            ptrdiff_t data_len_max,
            struct Arena* perm,
            struct Str* data_out,
            struct Str operation,
            struct Log* log) {
  (void)os;
  const struct FsReport report = {operation, log};
  int fd = -1;
  struct stat st;
  if (open_regular(file_path, &fd, &st, &report) != 0) {
    return -1;
  }
  if (st.st_size > data_len_max) {
    // Check the limit against the `fstat` size, before anything is allocated or read, so an
    // oversize file costs one `fstat` rather than its whole size in memory. A file that grows past
    // the limit after this check is still caught, because `os_read_bytes_exact` rejects any byte
    // beyond this size.
    (void)log_error(log, "%.*s: exceeds max file size (%td bytes) at %jd bytes ('%.*s')",
                    STR_FMT(operation), data_len_max, (intmax_t)st.st_size, STR_FMT(file_path));
    // The failure above is the one reported, so a close error on this path adds nothing.
    (void)close(fd);
    return -1;
  }
  // The size is within `data_len_max`, so it converts to `ptrdiff_t` exactly. Allocating from a
  // copy of `perm` and publishing it only on success leaves `perm` as it found it when the read
  // fails.
  const ptrdiff_t data_len = (ptrdiff_t)st.st_size;
  struct Arena arena = *perm;
  // The read overwrites every byte, so the bytes are not zeroed first: an empty `arena_alloc`
  // places them and `arena_extend` takes them as they are, which saves a pass over every byte read.
  (void)arena_alloc(&arena, 0, 1, 1);
  char* data = arena_extend(&arena, data_len, 1);
  if (os_read_bytes(data, data_len, fd, file_path, &report) != 0) {
    return -1;
  }
  *perm = arena;
  // `os_read_bytes` succeeds only on a full read, so the byte count is the stat size.
  *data_out = (struct Str){data, data_len};
  return 0;
}

int os_write(struct Os* os,
             struct Str file_path,
             struct OsSource source,
             struct Str operation,
             struct Log* log) {
  (void)os;
  const struct FsReport report = {operation, log};
  // A file source is opened first, so a source that is missing or not regular creates no parent
  // directory and no destination.
  int source_fd = -1;
  struct stat source_st;
  if (source.kind == OS_SOURCE_FILE &&
      open_regular(source.file_path, &source_fd, &source_st, &report) != 0) {
    return -1;
  }
  struct FsWriteTarget target = {.file_path = file_path, .fd = -1};
  // A path with no `/` has no parent to create, and one whose only `/` is the leading one has the
  // root, which always exists. Both yield an empty parent, which `mkdirs_report` treats as a no-op.
  int rc = mkdirs_report(path_parent(file_path), &report);
  if (rc == 0) {
    rc = os_write_open(&target, &report);
  }
  if (rc == 0) {
    rc = os_write_source(target.fd, source, source_fd, file_path, &report);
  }
  return os_write_close(&target, source_fd, source.file_path, rc, &report);
}

int os_mkdirs(struct Os* os, struct Str dir_path, struct Str operation, struct Log* log) {
  (void)os;
  const struct FsReport report = {operation, log};
  return mkdirs_report(dir_path, &report);
}

int os_remove(struct Os* os, struct Str file_path, struct Str operation, struct Log* log) {
  (void)os;
  const struct FsReport report = {operation, log};
  char path[PATH_MAX];
  if (copy_cstr(file_path, path) != 0) {
    return report_errno(&report, file_path, errno);
  }
  if (unlink(path) != 0 && errno != ENOENT) {
    return report_errno(&report, file_path, errno);
  }
  return 0;
}

int os_remove_dir(struct Os* os, struct Str dir_path, struct Str operation, struct Log* log) {
  (void)os;
  const struct FsReport report = {operation, log};
  char path[PATH_MAX];
  if (copy_cstr(dir_path, path) != 0) {
    return report_errno(&report, dir_path, errno);
  }
  // Some systems' `rmdir` removes the directory a symlink names when the path ends in `/`, so the
  // trailing `/` characters go, and a symlink fails as `ENOTDIR` with or without them.
  for (ptrdiff_t len = dir_path.len; len > 1 && path[len - 1] == '/'; len--) {
    path[len - 1] = '\0';
  }
  if (rmdir(path) != 0 && errno != ENOENT) {
    return report_errno(&report, dir_path, errno);
  }
  return 0;
}

int os_list(struct Os* os,
            struct Str dir_path,
            struct Arena* perm,
            struct OsEntry** entries_out,
            ptrdiff_t* count_out,
            struct Str operation,
            struct Log* log) {
  (void)os;
  const struct FsReport report = {operation, log};
  // Every name is read and the directory closed before any entry is inspected, so a caller that
  // recurses into an entry holds no directory open while it does.
  struct Arena arena = *perm;
  struct PathList names = {0};
  if (os_list_names(&names, dir_path, &arena, &report) != 0) {
    return -1;
  }
  struct OsEntry* entries = arena_new(&arena, names.count, struct OsEntry);
  ptrdiff_t count = 0;
  for (ptrdiff_t i = 0; i < names.count; i++) {
    // The joined path is needed only for the inspection, so it goes in a copy of the arena that the
    // next entry reuses.
    struct Arena join_arena = arena;
    const struct Str file_path = path_join(dir_path, names.items[i], &join_arena);
    enum OsEntryKind kind = OS_ENTRY_FILE;
    const enum ListClassify classified = os_list_classify(file_path, &kind);
    if (classified == LIST_CLASSIFY_FAILED) {
      // A file is there and could not be inspected: `EACCES` from a parent directory that is
      // readable but not searchable, `ENAMETOOLONG`, `EIO`. Skipping it would drop a file from the
      // build and still report success, so it is listed for the caller to report.
      entries[count] = (struct OsEntry){names.items[i], OS_ENTRY_FAILED, errno};
      count++;
    } else if (classified == LIST_CLASSIFY_LISTED) {
      entries[count] = (struct OsEntry){names.items[i], kind, 0};
      count++;
    }
  }
  *perm = arena;
  *entries_out = entries;
  *count_out = count;
  return 0;
}

int os_stat(struct Os* os, struct Str file_path, struct OsStat* stat_out) {
  (void)os;
  char path[PATH_MAX];
  struct stat st;
  if (copy_cstr(file_path, path) != 0 || stat(path, &st) != 0) {
    return -1;
  }
  if (is_slash_on_non_dir(file_path, st.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  stat_out->identity = (struct OsIdentity){(uint64_t)st.st_dev, (uint64_t)st.st_ino};
  stat_out->is_dir = S_ISDIR(st.st_mode);
  // A size beyond `PTRDIFF_MAX` cannot occur on a 64-bit system, and on a 32-bit one it is clamped,
  // which no size limit accepts anyway.
  stat_out->len = st.st_size < PTRDIFF_MAX ? (ptrdiff_t)st.st_size : PTRDIFF_MAX;
#if defined(__APPLE__)
  stat_out->mtime_s = (int64_t)st.st_mtimespec.tv_sec;
  stat_out->mtime_ns = (int64_t)st.st_mtimespec.tv_nsec;
#else
  stat_out->mtime_s = (int64_t)st.st_mtim.tv_sec;
  stat_out->mtime_ns = (int64_t)st.st_mtim.tv_nsec;
#endif
  return 0;
}

int os_print(struct Os* os, int fd, struct Str text) {
  (void)os;
  return write_all(fd, text);
}

static int os_read_bytes(char* data,
                         ptrdiff_t data_len,
                         int fd,
                         struct Str file_path,
                         const struct FsReport* report) {
  FILE* fp = fdopen(fd, "rb");
  if (fp == NULL) {
    (void)report_errno(report, file_path, errno);
    // The failure above is the one reported, so a close error on this path adds nothing.
    (void)close(fd);
    return -1;
  }
  int rc = os_read_bytes_exact(data, data_len, fp, file_path, report);
  if (fclose(fp) != 0) {
    if (rc == 0) {
      (void)report_errno(report, file_path, errno);
    }
    rc = -1;
  }
  return rc;
}

static int os_read_bytes_exact(char* data,
                               ptrdiff_t data_len,
                               FILE* fp,
                               struct Str file_path,
                               const struct FsReport* report) {
  assert(data_len >= 0);
  // Reset `errno` so a value left by an earlier call cannot pass for the cause of this one. ISO C
  // does not require `fread` to set it, so a stream error that left it at 0 is reported as `EIO`.
  errno = 0;
  const size_t read_len = fread(data, 1, (size_t)data_len, fp);
  // Capture `errno` before calling `ferror`, which is permitted to modify it even when it succeeds.
  // Reading it afterwards could name a cause the read never had.
  const int read_errno = errno;
  int rc = -1;
  if (ferror(fp) != 0) {
    (void)report_errno(report, file_path, read_errno == 0 ? EIO : read_errno);
  } else if (read_len != (size_t)data_len) {
    // No stream error, so the bytes ran out. The file shrank after the caller's `fstat`.
    (void)log_error(report->log, "%.*s: shrank while being read ('%.*s')",
                    STR_FMT(report->operation), STR_FMT(file_path));
  } else if (feof(fp) != 0) {
    // The read already consumed the whole file, so it cannot have grown.
    rc = 0;
  } else {
    // `fread` stops at `data_len`, so a file that grew between the caller's `fstat` and here would
    // read as a silently truncated copy. One more byte tells the two apart. Nothing left to read
    // means the size still matches, while any byte at all means the file changed underneath us.
    char extra = 0;
    errno = 0;
    const size_t extra_read = fread(&extra, 1, 1, fp);
    const int extra_errno = errno;
    if (ferror(fp) != 0) {
      (void)report_errno(report, file_path, extra_errno == 0 ? EIO : extra_errno);
    } else if (extra_read == 0) {
      rc = 0;
    } else {
      (void)log_error(report->log, "%.*s: grew while being read ('%.*s')",
                      STR_FMT(report->operation), STR_FMT(file_path));
    }
  }
  return rc;
}

static int os_write_open(struct FsWriteTarget* target, const struct FsReport* report) {
  if (copy_cstr(target->file_path, target->path) != 0) {
    return report_errno(report, target->file_path, errno);
  }
  // Mode `0666` is a ceiling the process umask trims, which is how every file this module creates
  // gets its permissions. An existing destination keeps its own mode, because `O_TRUNC` writes
  // through the file that is already there.
  target->fd = open(target->path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  return target->fd >= 0 ? 0 : report_errno(report, target->file_path, errno);
}

static int os_write_source(int fd,
                           struct OsSource source,
                           int source_fd,
                           struct Str file_path,
                           const struct FsReport* report) {
  switch (source.kind) {
    case OS_SOURCE_BYTES:
      return write_all(fd, source.bytes) == 0 ? 0 : report_errno(report, file_path, errno);
    case OS_SOURCE_FILE:
      return os_write_source_stream(fd, source_fd, source.file_path, file_path, report);
  }
  return -1;
}

static int os_write_source_stream(int fd,
                                  int source_fd,
                                  struct Str source_path,
                                  struct Str file_path,
                                  const struct FsReport* report) {
  char buffer[FS_WRITE_BUFFER_SIZE];
  for (;;) {
    const ssize_t read_len = read(source_fd, buffer, sizeof(buffer));
    if (read_len > 0) {
      if (write_all(fd, (struct Str){buffer, read_len}) != 0) {
        return report_errno(report, file_path, errno);
      }
    } else if (read_len == 0) {
      return 0;
    } else if (errno != EINTR) {
      return report_errno(report, source_path, errno);
    }
  }
}

static int os_write_close(const struct FsWriteTarget* target,
                          int source_fd,
                          struct Str source_path,
                          int rc,
                          const struct FsReport* report) {
  // Only the first failure is reported, because a later one is a consequence of it.
  if (target->fd >= 0 && close(target->fd) != 0) {
    if (rc == 0) {
      (void)report_errno(report, target->file_path, errno);
    }
    rc = -1;
  }
  if (source_fd >= 0 && close(source_fd) != 0) {
    if (rc == 0) {
      (void)report_errno(report, source_path, errno);
    }
    rc = -1;
  }
  return rc;
}

static int mkdirs_report(struct Str dir_path, const struct FsReport* report) {
  // Each `/` past the first byte ends one ancestor to create, and the whole path is the last.
  // Starting at the second byte keeps a leading `/` from becoming `mkdirs_report_one("")`, which
  // would fail with `ENOENT` on a root that always exists.
  for (ptrdiff_t i = 1; i < dir_path.len; i++) {
    if (dir_path.data[i] == '/' && mkdirs_report_one(str_head(dir_path, i), report) != 0) {
      return -1;
    }
  }
  return dir_path.len == 0 ? 0 : mkdirs_report_one(dir_path, report);
}

static int mkdirs_report_one(struct Str dir_path, const struct FsReport* report) {
  // Create first and treat `EEXIST` as success, rather than testing with `stat` and then creating.
  // Between a test and the create another process or a concurrent worker thread writing a sibling
  // output can win the race, and `os_mkdirs` runs this once per path component. Mode `0775` is a
  // ceiling the process umask trims, not the mode the directory ends up with. The `stat` below runs
  // only once `EEXIST` says something is already there, and rejects it when it is not a directory.
  char path[PATH_MAX];
  if (copy_cstr(dir_path, path) != 0) {
    return report_step_errno(report, S("create directory"), dir_path, errno);
  }
  if (mkdir(path, 0775) == 0) {
    return 0;
  }
  if (errno != EEXIST) {
    return report_step_errno(report, S("create directory"), dir_path, errno);
  }
  struct stat st;
  if (stat(path, &st) != 0) {
    return report_step_errno(report, S("inspect directory"), dir_path, errno);
  }
  if (!S_ISDIR(st.st_mode)) {
    // This names no step, unlike the two branches above. It needs no verb because "exists and is
    // not a directory" already says a directory was wanted. The errno branches do carry one,
    // because a caller may be writing a file rather than creating a directory, and there the verb
    // is what separates a failed parent directory from a failed write.
    return log_error(report->log, "%.*s: exists and is not a directory ('%.*s')",
                     STR_FMT(report->operation), STR_FMT(dir_path));
  }
  return 0;
}

static int os_list_names(struct PathList* names,
                         struct Str dir_path,
                         struct Arena* arena,
                         const struct FsReport* report) {
  char path[PATH_MAX];
  DIR* dp = copy_cstr(dir_path, path) == 0 ? opendir(path) : NULL;
  if (dp == NULL) {
    return report_step_errno(report, S("open directory"), dir_path, errno);
  }
  int rc = 0;
  while (rc == 0) {
    // `readdir` returns `NULL` both at end-of-directory and on error, so reset `errno` first to
    // tell them apart. A `NULL` entry with `errno` unchanged is the end. Otherwise the read failed.
    errno = 0;
    const struct dirent* entry = readdir(dp);
    if (entry == NULL) {
      if (errno != 0) {
        rc = report_step_errno(report, S("read directory"), dir_path, errno);
      }
      break;
    }
    const struct Str name = str_from_cstr(entry->d_name);
    if (!str_eq(name, S(".")) && !str_eq(name, S(".."))) {
      path_list_push(names, name, arena);
    }
  }
  if (closedir(dp) != 0) {
    if (rc == 0) {
      (void)report_step_errno(report, S("close directory"), dir_path, errno);
    }
    rc = -1;
  }
  return rc;
}

static enum ListClassify os_list_classify(struct Str file_path, enum OsEntryKind* kind_out) {
  char path[PATH_MAX];
  struct stat st;
  if (copy_cstr(file_path, path) != 0) {
    return LIST_CLASSIFY_FAILED;
  }
  // `lstat` first tells a symlink apart from what it names. A regular entry needs nothing more, so
  // only a symlink costs a second call.
  //
  // Either call failing skips an entry that resolves to nothing rather than listing it as failed: a
  // dangling symlink or one removed since `readdir` (`ENOENT`), or a symlink that resolves in a
  // cycle (`ELOOP`). No file exists behind either. `ENOENT` in particular is routine, not
  // exceptional. `readdir` and `stat` cannot be atomic, so any concurrent delete lands here and
  // must not fail the build. A symlink through a regular file fails as `ENOTDIR`, which is not
  // skipped, so it is listed as `OS_ENTRY_FAILED` as `os.h` documents.
  if (lstat(path, &st) != 0) {
    return errno == ENOENT || errno == ELOOP ? LIST_CLASSIFY_SKIPPED : LIST_CLASSIFY_FAILED;
  }
  const bool is_link = S_ISLNK(st.st_mode);
  if (is_link && stat(path, &st) != 0) {
    return errno == ENOENT || errno == ELOOP ? LIST_CLASSIFY_SKIPPED : LIST_CLASSIFY_FAILED;
  }
  if (S_ISDIR(st.st_mode)) {
    *kind_out = is_link ? OS_ENTRY_DIR_LINK : OS_ENTRY_DIR;
    return LIST_CLASSIFY_LISTED;
  }
  if (S_ISREG(st.st_mode)) {
    *kind_out = OS_ENTRY_FILE;
    return LIST_CLASSIFY_LISTED;
  }
  return LIST_CLASSIFY_SKIPPED;
}

static int open_regular(struct Str file_path,
                        int* fd_out,
                        struct stat* st_out,
                        const struct FsReport* report) {
  char path[PATH_MAX];
  const int fd =
      copy_cstr(file_path, path) == 0 ? open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC) : -1;
  if (fd < 0) {
    return report_errno(report, file_path, errno);
  }
  int rc = 0;
  if (fstat(fd, st_out) != 0) {
    rc = report_errno(report, file_path, errno);
  } else if (is_slash_on_non_dir(file_path, st_out->st_mode)) {
    rc = report_errno(report, file_path, ENOTDIR);
  } else if (!S_ISREG(st_out->st_mode)) {
    rc = log_error(report->log, "%.*s: not a regular file ('%.*s')", STR_FMT(report->operation),
                   STR_FMT(file_path));
  }
  if (rc == 0 && fcntl(fd, F_SETFL, 0) != 0) {
    rc = report_errno(report, file_path, errno);
  }
  if (rc != 0) {
    // The failure above is the one reported, so a close error on this path adds nothing.
    (void)close(fd);
    return -1;
  }
  *fd_out = fd;
  return 0;
}

static int write_all(int fd, struct Str data) {
  while (data.len > 0) {
    const ssize_t written_len =
        write(fd, data.data, (size_t)(data.len < FS_WRITE_LEN_MAX ? data.len : FS_WRITE_LEN_MAX));
    if (written_len > 0) {
      data = str_tail(data, written_len);
    } else if (written_len < 0 && errno == EINTR) {
      continue;
    } else if (written_len < 0) {
      return -1;
    } else {
      errno = EIO;
      return -1;
    }
  }
  return 0;
}

static int copy_cstr(struct Str path, char cstr_out[static PATH_MAX]) {
  // The lower bound is the `struct Str` invariant, spelled out so the copy below is visibly in
  // bounds in a build without assertions.
  if (path.len < 0 || path.len >= PATH_MAX) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (!text_is_nul_free(path)) {
    errno = EINVAL;
    return -1;
  }
  // `memcpy` requires valid pointers even for zero bytes, and an empty path may hold `NULL`.
  if (path.len > 0) {
    memcpy(cstr_out, path.data, (size_t)path.len);
  }
  cstr_out[path.len] = '\0';
  return 0;
}

static bool is_slash_on_non_dir(struct Str path, mode_t mode) {
  return str_endswith(path, S("/")) && !S_ISDIR(mode);
}

static int report_errno(const struct FsReport* report, struct Str path, int error_number) {
  char message[ERROR_SYSTEM_MESSAGE_SIZE];
  return log_error(report->log, "%.*s: %s ('%.*s')", STR_FMT(report->operation),
                   error_system_message(message, sizeof(message), error_number), STR_FMT(path));
}

static int report_step_errno(const struct FsReport* report,
                             struct Str step,
                             struct Str path,
                             int error_number) {
  char message[ERROR_SYSTEM_MESSAGE_SIZE];
  return log_error(report->log, "%.*s: cannot %.*s: %s ('%.*s')", STR_FMT(report->operation),
                   STR_FMT(step), error_system_message(message, sizeof(message), error_number),
                   STR_FMT(path));
}
