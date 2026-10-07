#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "runtime/fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "core/ascii.h"
#include "core/error.h"
#include "core/grow.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/text.h"
#include "shared/arena.h"

/** First slot count allocated when a walk records its first directory. */
enum { FS_WALK_VISITED_CAPACITY_MIN = 16 };

/** State that one directory walk carries from its root through every directory it reaches. */
struct FsWalk {
  /** Path list that receives the matching paths, unsorted until the walk ends. */
  struct PathList* paths;

  /** Array of `suffix_count` terminated suffixes to match. */
  const char* const* suffixes;

  /** Number of entries in `suffixes`. Zero matches nothing. */
  size_t suffix_count;

  /** Whether matching folds ASCII case. */
  bool is_fold_case;

  /**
   * Heap array of `visited_capacity` slots. The first `visited_count` hold the identity of every
   * directory walked so far and of the excluded directory, sorted by device and then inode, so a
   * directory reached a second time through a symlink, or the excluded one reached at all, is found
   * by binary search and skipped.
   */
  struct FsIdentity* visited;

  /** Number of populated entries in `visited`. */
  size_t visited_count;

  /** Allocated slots in `visited`. */
  size_t visited_capacity;

  /**
   * Symlinked directories found so far, in the order the walk found them. The walk reaches them
   * only after walking the root's own tree without following a symlink, so a directory with a path
   * of its own in that tree is listed under that path rather than under an alias to it.
   */
  struct PathList links;
};

/**
 * @brief Walks one directory, appending matching files to `walk->paths` without sorting.
 *
 * Skips a directory the walk has already visited. Otherwise lists the directory's entries, closes
 * it, and visits the entries in byte order, recursing into each real subdirectory and deferring
 * each symlinked one to `walk->links`. Closing before recursing holds one directory stream open at
 * a time, whatever the depth of the tree.
 *
 * @param walk       Walk state. Must not be `NULL`.
 * @param dir_path   Directory to walk. The tree root, a deferred symlink, or a real subdirectory
 *                   being scanned. Must not be `NULL`.
 * @param reason     Receives the failure reason, naming the directory that failed. May be `NULL`
 *                   only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, including a skipped directory, or `-1` on a directory or allocation
 *         failure.
 */
static int fs_list_files_with_suffixes_inner(struct FsWalk* walk,
                                             const char* dir_path,
                                             char* reason,
                                             size_t reason_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Records `excluded_dir` as already walked, so every path that reaches it is skipped.
 *
 * @param walk         Walk state whose visited set is seeded. Must not be `NULL`.
 * @param excluded_dir Directory to leave out of the walk, or `NULL` for none. A path with no
 *                     identity records nothing, because nothing below it can be listed.
 * @param reason       Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len   Size of `reason` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int fs_list_files_with_suffixes_exclude(struct FsWalk* walk,
                                               const char* excluded_dir,
                                               char* reason,
                                               size_t reason_len) __attribute__((nonnull(1)));

/**
 * @brief Records one directory identity as visited, reporting whether it was already recorded.
 *
 * @param walk       Walk state whose visited set is searched and extended. Must not be `NULL`.
 * @param identity   Identity of the directory about to be walked or excluded. Must not be `NULL`.
 * @param is_new_out Receives `true` when `identity` had not been visited and is now recorded, or
 *                   `false` when it had. Written only on success. Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int fs_list_files_with_suffixes_record(struct FsWalk* walk,
                                              const struct FsIdentity* identity,
                                              bool* is_new_out,
                                              char* reason,
                                              size_t reason_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Reads every entry name in `dir_path` except `.` and `..`, sorted in byte order.
 *
 * @param names      Initialized path list that receives the bare entry names. Must not be `NULL`.
 * @param dir_path   Directory to read. Must not be `NULL`.
 * @param reason     Receives the failure reason, naming `dir_path`. May be `NULL` only when
 *                   `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` on an open, read, close, or allocation failure.
 */
static int read_dir_names(struct PathList* names,
                          const char* dir_path,
                          char* reason,
                          size_t reason_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Appends one matching regular file, descends into a real subdirectory, or defers a
 *        symlinked one.
 *
 * @param walk       Walk state. Must not be `NULL`.
 * @param dir_path   Directory containing `entry_name`. Must not be `NULL`.
 * @param entry_name Bare directory entry name to inspect. Must not be `NULL`.
 * @param scratch    Arena owning the joined path for this entry. Owned by the enclosing directory's
 *                   walk, which outlives any recursion into this entry. Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success (including skipped entries), or `-1` on failure.
 */
static int visit_matching_entry(struct FsWalk* walk,
                                const char* dir_path,
                                const char* entry_name,
                                struct Arena* scratch,
                                char* reason,
                                size_t reason_len) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Compares two directory identities by device and then inode.
 *
 * @param a First identity. Must not be `NULL`.
 * @param b Second identity. Must not be `NULL`.
 * @return A negative, zero, or positive value as `a` sorts before, equal to, or after `b`.
 */
static int compare_identities(const struct FsIdentity* a, const struct FsIdentity* b)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether `text` ends with the literal `suffix`.
 *
 * @param text         Terminated string to test. Must not be `NULL`.
 * @param suffix       Terminated suffix to look for. Must not be `NULL`.
 * @param is_fold_case Whether ASCII case differences are ignored, so `.JpG` matches `.jpg`. Bytes
 *                     outside ASCII always compare exactly.
 * @return `true` when `text` ends with `suffix`, `false` otherwise.
 */
static bool has_suffix(const char* text, const char* suffix, bool is_fold_case)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether `text` ends with any of `suffixes`.
 *
 * @param text         Terminated string to test. Must not be `NULL`.
 * @param suffixes     Array of `suffix_count` terminated suffixes. Must not be `NULL`.
 * @param suffix_count Number of entries in `suffixes`. Zero matches nothing.
 * @param is_fold_case Whether ASCII case differences are ignored, as in `has_suffix`.
 * @return `true` when `text` ends with at least one suffix, `false` otherwise.
 */
static bool has_any_suffix(const char* text,
                           const char* const* suffixes,
                           size_t suffix_count,
                           bool is_fold_case) __attribute__((nonnull(1, 2)));

/**
 * @brief Compares two `PathList` item pointers for `qsort`.
 *
 * @param a Pointer to the first `char*` element. Must not be `NULL`.
 * @param b Pointer to the second `char*` element. Must not be `NULL`.
 * @return A negative, zero, or positive value per `strcmp` of the two paths.
 */
static int compare_paths(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Reads exactly `data_len` bytes into `data`, verifying the file did not change underneath.
 *
 * `fstat` and `fread` are two observations of a file another process may be writing, so a read that
 * returns anything other than `data_len` bytes is a changed file rather than a partial success.
 * Both directions are rejected: too few bytes means the file shrank, and one readable byte past
 * `data_len` means it grew. Byte values have no policy here; text consumers validate separately.
 *
 * @param data       Buffer of at least `data_len` bytes that receives the file contents. Must not
 *                   be `NULL`.
 * @param data_len   Number of bytes to read, taken from the `fstat` of the open file.
 * @param fp         Stream positioned at the start of the file. Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` when exactly `data_len` stable bytes were read, or `-1` otherwise.
 */
static int fs_read_file_bytes(unsigned char* data,
                              size_t data_len,
                              FILE* fp,
                              char* reason,
                              size_t reason_len) __attribute__((nonnull(1, 3)));

/**
 * @brief Creates the parent directory of `file_path` if it has one.
 *
 * @param file_path  Path whose parent directory should exist. Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success or when the path has no directory component, or `-1` on failure.
 */
static int ensure_parent_dir(const char* file_path, char* reason, size_t reason_len)
    __attribute__((nonnull(1)));

/**
 * @brief Creates one directory, accepting a path that already names a directory.
 *
 * Creates only the named directory. `fs_mkdir_p` walks the path and calls this per component.
 *
 * @param dir_path   Directory path to create. Must not be `NULL`.
 * @param reason     Receives the failure reason, naming `dir_path` since it is one component of a
 *                   path the caller named whole. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success or when `dir_path` already names a directory, or `-1` on failure.
 */
static int ensure_dir(const char* dir_path, char* reason, size_t reason_len)
    __attribute__((nonnull(1)));

/**
 * @brief Writes every byte to an open descriptor, retrying interrupted and short writes.
 *
 * @param fd         Descriptor open for writing.
 * @param data       Bytes to write. Must hold at least `data_len` bytes. Must not be `NULL`.
 * @param data_len   Number of bytes to write.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` once all `data_len` bytes are written, or `-1` on a write error or a write that makes
 *         no progress.
 */
static int write_all(int fd, const void* data, size_t data_len, char* reason, size_t reason_len)
    __attribute__((nonnull(2)));

/**
 * @brief Reports `error_number` alone as a failure reason.
 *
 * This is for a failure on the path the caller already names, where repeating it would only pad the
 * composed diagnostic.
 *
 * @param reason       Receives the reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len   Size of `reason` in bytes.
 * @param error_number `errno` value to describe.
 * @return `-1` always, so a caller can write `return fs_reason_errno(...);`.
 */
static int fs_reason_errno(char* reason, size_t reason_len, int error_number);

/**
 * @brief Reports `error_number` as a failure reason naming the operation and path that failed.
 *
 * This is for a failure on a path the caller cannot name, such as a directory reached partway
 * through a recursive walk or one component of a path created whole.
 *
 * @param reason       Receives the reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len   Size of `reason` in bytes.
 * @param action       Lowercase operation that failed, such as `open directory`. Must not be
 *                     `NULL`.
 * @param path         Path the operation was attempted on. Must not be `NULL`.
 * @param error_number `errno` value to describe.
 * @return `-1` always, so a caller can write `return fs_reason_path_errno(...);`.
 */
static int fs_reason_path_errno(char* reason,
                                size_t reason_len,
                                const char* action,
                                const char* path,
                                int error_number) __attribute__((nonnull(3, 4)));

int fs_list_files_with_suffixes(struct PathList* paths,
                                const char* root_dir,
                                const char* excluded_dir,
                                const char* const* suffixes,
                                size_t suffix_count,
                                bool is_fold_case,
                                char* reason,
                                size_t reason_len) {
  struct FsWalk walk = {.paths = paths,
                        .suffixes = suffixes,
                        .suffix_count = suffix_count,
                        .is_fold_case = is_fold_case};
  path_list_init(&walk.links);
  int rc = fs_list_files_with_suffixes_exclude(&walk, excluded_dir, reason, reason_len);
  if (rc == 0) {
    rc = fs_list_files_with_suffixes_inner(&walk, root_dir, reason, reason_len);
  }
  // A deferred link can defer more links of its own, so this reads `count` on every iteration
  // rather than once.
  for (size_t i = 0; rc == 0 && i < walk.links.count; i++) {
    rc = fs_list_files_with_suffixes_inner(&walk, walk.links.items[i], reason, reason_len);
  }
  free(walk.visited);
  path_list_free(&walk.links);
  // Skip the empty case rather than let `qsort` see it. `path_list_init` leaves `items` `NULL`, and
  // passing a null pointer to `qsort` is undefined even with a count of 0.
  if (rc == 0 && paths->count > 0) {
    qsort(paths->items, paths->count, sizeof(*paths->items), compare_paths);
  }
  return rc;
}

int fs_read_file(const char* file_path,
                 size_t data_len_max,
                 unsigned char** data_out,
                 size_t* data_len_out,
                 char* reason,
                 size_t reason_len) {
  // Open without blocking and inspect the descriptor rather than the path, so the file checked is
  // the file read. A path swapped to a FIFO would otherwise block the open waiting for a writer.
  // The flag is cleared again once the file is known to be regular.
  const int fd = open(file_path, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    return fs_reason_errno(reason, reason_len, errno);
  }
  struct stat st;
  int rc = 0;
  if (fstat(fd, &st) != 0) {
    rc = fs_reason_errno(reason, reason_len, errno);
  } else if (!S_ISREG(st.st_mode)) {
    rc = error_report(reason, reason_len, "not a regular file");
  } else if ((uintmax_t)st.st_size > (uintmax_t)data_len_max) {
    // Check the limit against the `fstat` size, before anything is allocated or read, so an
    // oversize file costs one `fstat` rather than its whole size in memory. A file that grows past
    // the limit after this check is still caught, because `fs_read_file_bytes` rejects any byte
    // beyond this size. `data_len_max` is below `SIZE_MAX` by contract, so passing this check also
    // keeps `size + 1` from wrapping to 0. Both sides widen to `uintmax_t` because `off_t` and
    // `size_t` may differ in width.
    rc = error_report(reason, reason_len, "exceeds max file size (%zu bytes) at %ju bytes",
                      data_len_max, (uintmax_t)st.st_size);
  }
  if (rc == 0 && fcntl(fd, F_SETFL, 0) != 0) {
    rc = fs_reason_errno(reason, reason_len, errno);
  }
  FILE* fp = rc == 0 ? fdopen(fd, "rb") : NULL;
  if (fp == NULL) {
    if (rc == 0) {
      (void)fs_reason_errno(reason, reason_len, errno);
    }
    // The failure above is the one reported, so a close error on this path adds nothing.
    (void)close(fd);
    return -1;
  }

  const size_t size = (size_t)st.st_size;

  // Use `malloc`, not `calloc`. `fs_read_file_bytes` writes all `size` bytes or fails, so only the
  // terminator needs to be zero, and zero-filling first would mean a second pass over the largest
  // file this program reads.
  unsigned char* data = malloc(size + 1);
  rc = -1;
  if (data == NULL) {
    (void)error_report(reason, reason_len, "out of memory");
  } else {
    data[size] = '\0';
    rc = fs_read_file_bytes(data, size, fp, reason, reason_len);
  }
  // Close before publishing so a close error fails the read and the buffer is still reclaimed.
  if (fclose(fp) != 0) {
    if (rc == 0) {
      (void)fs_reason_errno(reason, reason_len, errno);
    }
    rc = -1;
  }
  if (rc == 0) {
    *data_out = data;
    // `fs_read_file_bytes` succeeds only on a full read, so the byte count is the stat size.
    *data_len_out = size;
    data = NULL;
  }

  free(data);
  return rc;
}

int fs_write_file(const char* file_path,
                  const void* data,
                  size_t data_len,
                  char* reason,
                  size_t reason_len) {
  if (ensure_parent_dir(file_path, reason, reason_len) != 0) {
    return -1;
  }
  // Mode `0666` is a ceiling the process umask trims, which is how every file this module creates
  // gets its permissions. An existing destination keeps its own mode, because `O_TRUNC` writes
  // through the file that is already there.
  const int fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    return fs_reason_errno(reason, reason_len, errno);
  }
  int rc = write_all(fd, data, data_len, reason, reason_len);
  if (close(fd) != 0) {
    if (rc == 0) {
      (void)fs_reason_errno(reason, reason_len, errno);
    }
    rc = -1;
  }
  return rc;
}

int fs_copy_file(const char* source_path, const char* dest_path, char* reason, size_t reason_len) {
  // Open without blocking so a FIFO reaches the regular-file check below instead of waiting for a
  // writer. The flag is cleared again once the source is known to be regular.
  const int source_fd = open(source_path, O_RDONLY | O_NONBLOCK);
  if (source_fd < 0) {
    return fs_reason_errno(reason, reason_len, errno);
  }
  struct stat source_st;
  int rc = 0;
  if (fstat(source_fd, &source_st) != 0) {
    rc = fs_reason_errno(reason, reason_len, errno);
  } else if (!S_ISREG(source_st.st_mode)) {
    rc = error_report(reason, reason_len, "not a regular file");
  } else if (ensure_parent_dir(dest_path, reason, reason_len) != 0) {
    rc = -1;
  }
  if (rc == 0 && fcntl(source_fd, F_SETFL, 0) != 0) {
    rc = fs_reason_errno(reason, reason_len, errno);
  }

  int dest_fd = -1;
  if (rc == 0) {
    // The copy deliberately does not carry the source's mode over. A published static file gets the
    // same `0666`-minus-umask treatment as every other file this module creates, rather than
    // inheriting whatever the source tree happened to have, which would let a `0600` stylesheet
    // publish unreadable.
    dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (dest_fd < 0) {
      rc = fs_reason_errno(reason, reason_len, errno);
    }
  }

  unsigned char buffer[64 * 1024];
  while (rc == 0) {
    const ssize_t nread = read(source_fd, buffer, sizeof(buffer));
    if (nread > 0) {
      rc = write_all(dest_fd, buffer, (size_t)nread, reason, reason_len);
    } else if (nread == 0) {
      break;
    } else if (errno != EINTR) {
      rc = fs_reason_errno(reason, reason_len, errno);
    }
  }
  if (close(source_fd) != 0) {
    if (rc == 0) {
      (void)fs_reason_errno(reason, reason_len, errno);
    }
    rc = -1;
  }
  if (dest_fd >= 0 && close(dest_fd) != 0) {
    if (rc == 0) {
      (void)fs_reason_errno(reason, reason_len, errno);
    }
    rc = -1;
  }
  return rc;
}

int fs_stat_meta(const char* file_path, struct FsMeta* meta_out, char* reason, size_t reason_len) {
  struct stat st;
  if (stat(file_path, &st) != 0) {
    return fs_reason_errno(reason, reason_len, errno);
  }
  if (!S_ISREG(st.st_mode)) {
    return error_report(reason, reason_len, "not a regular file");
  }
  // A regular file's size is never negative. The `#if` omits the width check where `size_t` is as
  // wide as `uintmax_t`, because there the comparison is a tautology rather than a bound.
#if SIZE_MAX < UINTMAX_MAX
  if ((uintmax_t)st.st_size > (uintmax_t)SIZE_MAX) {
    return error_report(reason, reason_len, "file size cannot be represented");
  }
#endif
  struct FsMeta meta = {.size_len = (size_t)st.st_size};
#if defined(__APPLE__)
  meta.mtime_s = (int64_t)st.st_mtimespec.tv_sec;
  meta.mtime_ns = (int64_t)st.st_mtimespec.tv_nsec;
#else
  meta.mtime_s = (int64_t)st.st_mtim.tv_sec;
  meta.mtime_ns = (int64_t)st.st_mtim.tv_nsec;
#endif
  *meta_out = meta;
  return 0;
}

int fs_mkdir_p(const char* dir_path, char* reason, size_t reason_len) {
  if (dir_path[0] == '\0') {
    return 0;
  }
  char* copy = text_strdup(dir_path);
  if (copy == NULL) {
    return error_report(reason, reason_len, "out of memory");
  }

  int rc = -1;
  // Start at the second byte so a leading `/` does not become `ensure_dir("")`, which would fail
  // with `ENOENT` on a root that always exists. The empty-path early return above keeps `copy + 1`
  // in bounds. For `""` it would point one past the terminator, and dereferencing it reads outside
  // the object.
  for (char* p = copy + 1;; p++) {
    if (*p != '/' && *p != '\0') {
      continue;
    }
    const char saved = *p;
    *p = '\0';
    if (ensure_dir(copy, reason, reason_len) != 0) {
      goto cleanup;
    }
    *p = saved;
    if (saved == '\0') {
      break;
    }
  }
  rc = 0;

cleanup:
  free(copy);
  return rc;
}

bool fs_path_exists(const char* path) {
  struct stat st;
  return stat(path, &st) == 0;
}

int fs_require_dir(const char* dir_path, char* reason, size_t reason_len) {
  struct stat st;
  if (stat(dir_path, &st) != 0) {
    // The bare system message rather than `fs_reason_path_errno`'s "cannot <action>" form: the
    // caller names the operation, so a verb here would stutter in the composed diagnostic.
    char message[FS_REASON_SIZE];
    return error_report(reason, reason_len, "%s ('%s')",
                        error_system_message(message, sizeof(message), errno), dir_path);
  }
  if (!S_ISDIR(st.st_mode)) {
    return error_report(reason, reason_len, "not a directory ('%s')", dir_path);
  }
  return 0;
}

int fs_identify(const char* file_path, struct FsIdentity* identity_out) {
  struct stat st;
  if (stat(file_path, &st) != 0) {
    return -1;
  }
  identity_out->device = (uint64_t)st.st_dev;
  identity_out->inode = (uint64_t)st.st_ino;
  return 0;
}

static int fs_list_files_with_suffixes_inner(struct FsWalk* walk,
                                             const char* dir_path,
                                             char* reason,
                                             size_t reason_len) {
  // This `stat` does not validate the type. `opendir` in `read_dir_names` rejects a non-directory,
  // which only the root can be, because `visit_matching_entry` passes on only entries it found to
  // be directories.
  struct stat st;
  if (stat(dir_path, &st) != 0) {
    return fs_reason_path_errno(reason, reason_len, "inspect directory", dir_path, errno);
  }
  const struct FsIdentity identity = {.device = (uint64_t)st.st_dev, .inode = (uint64_t)st.st_ino};
  bool is_new = false;
  if (fs_list_files_with_suffixes_record(walk, &identity, &is_new, reason, reason_len) != 0) {
    return -1;
  }
  if (!is_new) {
    // This silently skips a directory the walk already reached by another path: a symlink back into
    // its own ancestry, or a second alias of one directory. Every file below it is already in
    // `paths` under the first path, so nothing is missing. Walking it again would list each file
    // once per path, and a chain of aliased directories multiplies that at every level. The
    // excluded directory is recorded before the walk starts, so it takes this skip on every path.
    return 0;
  }

  struct PathList names;
  path_list_init(&names);
  int rc = read_dir_names(&names, dir_path, reason, reason_len);

  // This uses one arena for the whole directory rather than one per entry. Every joined path here
  // is a few hundred bytes, while an arena's first chunk is 8 KB, so a per-entry arena would mean
  // an 8 KB `malloc`/`free` pair for every file in the tree. The lifetime ends with this directory,
  // which is short enough to bound the memory and long enough for a subdirectory's path to stay
  // valid while the recursion below uses it as a root.
  struct Arena scratch;
  arena_init(&scratch);
  for (size_t i = 0; rc == 0 && i < names.count; i++) {
    rc = visit_matching_entry(walk, dir_path, names.items[i], &scratch, reason, reason_len);
  }
  arena_free(&scratch);
  path_list_free(&names);
  return rc;
}

static int fs_list_files_with_suffixes_exclude(struct FsWalk* walk,
                                               const char* excluded_dir,
                                               char* reason,
                                               size_t reason_len) {
  struct FsIdentity identity;
  if (excluded_dir == NULL || fs_identify(excluded_dir, &identity) != 0) {
    return 0;
  }
  bool is_new = false;
  return fs_list_files_with_suffixes_record(walk, &identity, &is_new, reason, reason_len);
}

static int fs_list_files_with_suffixes_record(struct FsWalk* walk,
                                              const struct FsIdentity* identity,
                                              bool* is_new_out,
                                              char* reason,
                                              size_t reason_len) {
  // Binary search for the first slot not below `identity`, which is where it is or belongs.
  size_t low = 0;
  size_t high = walk->visited_count;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    if (compare_identities(&walk->visited[mid], identity) < 0) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  if (low < walk->visited_count && compare_identities(&walk->visited[low], identity) == 0) {
    *is_new_out = false;
    return 0;
  }

  if (walk->visited_count == walk->visited_capacity) {
    size_t capacity_next = 0;
    size_t capacity_bytes_next = 0;
    if (grow_capacity(walk->visited_capacity, FS_WALK_VISITED_CAPACITY_MIN, sizeof(*walk->visited),
                      &capacity_next, &capacity_bytes_next) != 0) {
      return error_report(reason, reason_len, "out of memory");
    }
    struct FsIdentity* visited = realloc(walk->visited, capacity_bytes_next);
    if (visited == NULL) {
      return error_report(reason, reason_len, "out of memory");
    }
    walk->visited = visited;
    walk->visited_capacity = capacity_next;
  }
  // Inserting in place keeps the array sorted. The shift is linear, but the walk records each
  // directory once, and a content tree has far fewer directories than files.
  memmove(&walk->visited[low + 1], &walk->visited[low],
          (walk->visited_count - low) * sizeof(*walk->visited));
  walk->visited[low] = *identity;
  walk->visited_count++;
  *is_new_out = true;
  return 0;
}

static int read_dir_names(struct PathList* names,
                          const char* dir_path,
                          char* reason,
                          size_t reason_len) {
  DIR* dp = opendir(dir_path);
  if (dp == NULL) {
    return fs_reason_path_errno(reason, reason_len, "open directory", dir_path, errno);
  }
  int rc = 0;
  while (rc == 0) {
    // `readdir` returns `NULL` both at end-of-directory and on error, so reset `errno` first to
    // tell them apart. A `NULL` entry with `errno` unchanged is the end. Otherwise the walk failed.
    errno = 0;
    const struct dirent* entry = readdir(dp);
    if (entry == NULL) {
      if (errno != 0) {
        rc = fs_reason_path_errno(reason, reason_len, "read directory", dir_path, errno);
      }
      break;
    }
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
        path_list_push(names, entry->d_name) != 0) {
      rc = error_report(reason, reason_len, "out of memory");
    }
  }
  if (closedir(dp) != 0) {
    if (rc == 0) {
      (void)fs_reason_path_errno(reason, reason_len, "close directory", dir_path, errno);
    }
    rc = -1;
  }
  // `readdir` order is whatever the filesystem stores. Sorting makes the walk order, and with it
  // which of two aliases of one directory is walked, the same on every machine.
  if (rc == 0 && names->count > 0) {
    qsort(names->items, names->count, sizeof(*names->items), compare_paths);
  }
  return rc;
}

static int visit_matching_entry(struct FsWalk* walk,
                                const char* dir_path,
                                const char* entry_name,
                                struct Arena* scratch,
                                char* reason,
                                size_t reason_len) {
  char* file_path = path_join(dir_path, entry_name, scratch);
  if (file_path == NULL) {
    return error_report(reason, reason_len, "out of memory");
  }
  // `lstat` first tells a symlink apart from what it names. A regular entry needs nothing more, so
  // only a symlink costs a second call.
  struct stat st;
  int stat_rc = lstat(file_path, &st);
  const bool is_link = stat_rc == 0 && S_ISLNK(st.st_mode);
  if (is_link) {
    stat_rc = stat(file_path, &st);
  }
  if (stat_rc != 0) {
    if (errno == ENOENT || errno == ELOOP) {
      // This skips an entry that resolves to nothing rather than aborting the whole walk: a
      // dangling symlink or one removed since `readdir` (`ENOENT`), or a symlink that resolves in a
      // cycle (`ELOOP`). No file exists behind either, so the walk loses nothing by stepping past
      // it. `ENOENT` in particular is routine, not exceptional. `readdir` and `stat` cannot be
      // atomic, so any concurrent delete lands here and must not fail the build.
      return 0;
    }
    // Every other reason means a file is there and could not be inspected: `EACCES` from a parent
    // directory that is readable but not searchable, `ENAMETOOLONG`, `EIO`. Skipping those would
    // drop a source media file from the build and still report success.
    return fs_reason_path_errno(reason, reason_len, "inspect entry", file_path, errno);
  }
  if (S_ISDIR(st.st_mode)) {
    if (is_link) {
      // A symlinked directory waits until every directory reachable without a symlink is walked, so
      // the target's own path, when it has one in the tree, is the one that lists its files.
      return path_list_push(&walk->links, file_path) == 0
                 ? 0
                 : error_report(reason, reason_len, "out of memory");
    }
    return fs_list_files_with_suffixes_inner(walk, file_path, reason, reason_len);
  }
  if (S_ISREG(st.st_mode) &&
      has_any_suffix(entry_name, walk->suffixes, walk->suffix_count, walk->is_fold_case)) {
    if (path_list_push(walk->paths, file_path) != 0) {
      return error_report(reason, reason_len, "out of memory");
    }
  }
  return 0;
}

static int compare_identities(const struct FsIdentity* a, const struct FsIdentity* b) {
  if (a->device != b->device) {
    return a->device < b->device ? -1 : 1;
  }
  if (a->inode != b->inode) {
    return a->inode < b->inode ? -1 : 1;
  }
  return 0;
}

static bool has_suffix(const char* text, const char* suffix, bool is_fold_case) {
  const size_t text_len = strlen(text);
  const size_t suffix_len = strlen(suffix);
  // The length test must short-circuit the pointer arithmetic. `text_len - suffix_len` wraps when
  // the suffix is the longer string, and `text +` that value forms a pointer far outside the
  // object, which is undefined regardless of whether it is dereferenced.
  if (text_len < suffix_len) {
    return false;
  }
  const char* candidate = text + text_len - suffix_len;
  for (size_t i = 0; i < suffix_len; i++) {
    const unsigned char left = (unsigned char)candidate[i];
    const unsigned char right = (unsigned char)suffix[i];
    if (left != right && (!is_fold_case || ascii_to_lower(left) != ascii_to_lower(right))) {
      return false;
    }
  }
  return true;
}

static bool has_any_suffix(const char* text,
                           const char* const* suffixes,
                           size_t suffix_count,
                           bool is_fold_case) {
  for (size_t i = 0; i < suffix_count; i++) {
    if (has_suffix(text, suffixes[i], is_fold_case)) {
      return true;
    }
  }
  return false;
}

static int compare_paths(const void* a, const void* b) {
  const char* const* pa = a;
  const char* const* pb = b;
  return strcmp(*pa, *pb);
}

static int fs_read_file_bytes(unsigned char* data,
                              size_t data_len,
                              FILE* fp,
                              char* reason,
                              size_t reason_len) {
  // Reset `errno` so a value left by an earlier call cannot pass for the cause of this one. ISO C
  // does not require `fread` to set it, so a stream error that left it at 0 is reported as `EIO`.
  errno = 0;
  const size_t nread = fread(data, 1, data_len, fp);
  // Capture `errno` before calling `ferror`, which is permitted to modify it even when it succeeds.
  // Reading it afterwards could name a cause the read never had.
  const int read_errno = errno;
  int rc = -1;
  if (ferror(fp) != 0) {
    (void)fs_reason_errno(reason, reason_len, read_errno == 0 ? EIO : read_errno);
  } else if (nread != data_len) {
    // No stream error, so the bytes ran out. The file shrank after the caller's `fstat`.
    (void)error_report(reason, reason_len, "shrank while being read");
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
      (void)fs_reason_errno(reason, reason_len, extra_errno == 0 ? EIO : extra_errno);
    } else if (extra_read == 0) {
      rc = 0;
    } else {
      (void)error_report(reason, reason_len, "grew while being read");
    }
  }
  return rc;
}

static int ensure_parent_dir(const char* file_path, char* reason, size_t reason_len) {
  const char* slash = strrchr(file_path, '/');
  if (slash == NULL) {
    return 0;
  }
  const size_t len = (size_t)(slash - file_path);
  // A path whose only `/` is the leading one yields a zero-length parent, which `fs_mkdir_p` treats
  // as a no-op. The parent is the root, which always exists.
  char* parent = malloc(len + 1);
  if (parent == NULL) {
    return error_report(reason, reason_len, "out of memory");
  }
  memcpy(parent, file_path, len);
  parent[len] = '\0';
  const int rc = fs_mkdir_p(parent, reason, reason_len);
  free(parent);
  return rc;
}

static int ensure_dir(const char* dir_path, char* reason, size_t reason_len) {
  // Create first and treat `EEXIST` as success, rather than testing with `stat` and then creating.
  // Between a test and the create another process or a concurrent worker thread writing a sibling
  // output can win the race, and `fs_mkdir_p` runs this once per path component. Mode `0775` is a
  // ceiling the process umask trims, not the mode the directory ends up with. The `stat` below runs
  // only once `EEXIST` says something is already there, and rejects it when it is not a directory.
  if (mkdir(dir_path, 0775) == 0) {
    return 0;
  }
  if (errno != EEXIST) {
    return fs_reason_path_errno(reason, reason_len, "create directory", dir_path, errno);
  }
  struct stat st;
  if (stat(dir_path, &st) != 0) {
    return fs_reason_path_errno(reason, reason_len, "inspect directory", dir_path, errno);
  }
  if (!S_ISDIR(st.st_mode)) {
    // This is a bare cause fragment, unlike the two branches above, which name the operation. It
    // needs no verb because "exists and is not a directory" already says a directory was wanted.
    // The errno branches do carry one, because a caller may be writing a file rather than creating
    // a directory, and there the verb is what separates a failed parent directory from a failed
    // write. The path trails the cause either way, so a deep component cannot truncate it.
    return error_report(reason, reason_len, "exists and is not a directory ('%s')", dir_path);
  }
  return 0;
}

static int write_all(int fd, const void* data, size_t data_len, char* reason, size_t reason_len) {
  const unsigned char* bytes = data;
  size_t offset = 0;
  while (offset < data_len) {
    const ssize_t nwritten = write(fd, bytes + offset, data_len - offset);
    if (nwritten > 0) {
      offset += (size_t)nwritten;
    } else if (nwritten < 0 && errno == EINTR) {
      continue;
    } else if (nwritten < 0) {
      return fs_reason_errno(reason, reason_len, errno);
    } else {
      return error_report(reason, reason_len, "write made no progress");
    }
  }
  return 0;
}

static int fs_reason_errno(char* reason, size_t reason_len, int error_number) {
  char message[FS_REASON_SIZE];
  return error_report(reason, reason_len, "%s",
                      error_system_message(message, sizeof(message), error_number));
}

static int fs_reason_path_errno(char* reason,
                                size_t reason_len,
                                const char* action,
                                const char* path,
                                int error_number) {
  char message[FS_REASON_SIZE];
  return error_report(reason, reason_len, "cannot %s: %s ('%s')", action,
                      error_system_message(message, sizeof(message), error_number), path);
}
