#ifndef FRAM_FS_H
#define FRAM_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct PathList;

/**
 * Identity of one existing file, as the device and inode pair naming it uniquely.
 *
 * Comparing identities answers "are these two paths the same file?" where comparing path strings
 * cannot. `./photos/x.jpg` and `photos/x.jpg` are the same file spelled two ways, and so are a
 * symlink and its target, a hard link and its twin, and two spellings that differ only in case on a
 * case-insensitive filesystem. These fields are wider than `dev_t` and `ino_t` so this header does
 * not put system types on its callers.
 */
struct FsIdentity {
  /** Device the file resides on. */
  uint64_t device;

  /** Inode number within `device`. */
  uint64_t inode;
};

/** Source metadata used to decide whether a generated output is current. */
struct FsMeta {
  /** Modification time in whole seconds since the Unix epoch. */
  int64_t mtime_s;

  /**
   * Nanoseconds within `mtime_s`. A freshness check compares an output against its source at this
   * resolution because a build regenerates an output in the same second its source changed, and a
   * whole-second comparison would then accept a stale output as current.
   */
  int64_t mtime_ns;

  /**
   * File size in bytes. A freshness check treats a zero-size output as stale, and a copied original
   * must additionally match its source's size.
   */
  size_t size_len;
};

/**
 * Size in bytes of a filesystem failure reason, including the `NUL` terminator.
 *
 * These actions report a reason fragment, not a whole diagnostic, because the caller owns the
 * operation and the attribution. One failed `fs_write_file` is `failed to write output` from the
 * album page write and `failed to write template output` from the aggregate write. First-party
 * fragments are lowercase and unquoted. Operating-system messages keep their original spelling.
 * Both compose as `<caller's operation>: <reason>`, with any unbounded path trailing the reason,
 * never leading it. `ERROR_MESSAGE_SIZE` is smaller than a path may be, so a leading path would
 * truncate the cause off the end. This is sized for a system message plus a directory path of
 * ordinary depth, because the caller cannot name the failing component. Only the walk knows how
 * deep it got.
 */
enum { FS_REASON_SIZE = 256 };

/**
 * @brief Recursively lists regular files under `root_dir` whose name ends with any of `suffixes`.
 *
 * Appends each matching file's path to `paths` and sorts the whole list so builds are reproducible.
 * A match may fold ASCII case, which lets `.JpG` match `.jpg` without making behavior
 * locale-dependent. An empty suffix matches every regular file, and a zero suffix count matches
 * none.
 *
 * It follows symlinked directories but walks each directory once, identified by device and inode,
 * so a file below a directory reachable by several paths is listed once, not once per path. A
 * symlink back into its own ancestry and a second alias of a directory are both skipped silently,
 * because every file below them is already listed. The listed path is the one reached without
 * following a symlink when the tree has one. Otherwise it is the first path through a symlink, in a
 * walk that visits entries in byte order and each symlinked directory only after every real one. It
 * likewise skips an entry that resolves to nothing: a dangling symlink, one removed since it was
 * read, or a symlink that resolves in a cycle. An entry that exists but cannot be inspected fails
 * the walk instead, so a file this function could not look at is never silently missing from
 * `paths`.
 *
 * It holds one directory open at a time, so the depth of the tree is bounded by path length rather
 * than by the open-file limit.
 *
 * It skips `excluded_dir` and everything below it, compared by identity rather than by path text,
 * so the skip holds on every path that reaches it: its own path in the tree, a symlinked alias, or
 * `root_dir` itself. A build passes its `output_dir` here for every input tree it walks, so a file
 * that an earlier build generated is never read back as an input, even through a symlink into the
 * output tree. An `excluded_dir` that does not exist yet holds no file and excludes nothing.
 *
 * @param paths        Initialized path list that receives the matching paths. Must not be `NULL`.
 * @param root_dir     Directory tree to walk. Must not be `NULL`.
 * @param excluded_dir Directory left out of the walk, or `NULL` to walk the whole tree. A path with
 *                     no identity excludes nothing.
 * @param suffixes     Array of `suffix_count` terminated suffixes, such as `.jpg`. Must not be
 *                     `NULL`.
 * @param suffix_count Number of suffixes. Zero matches no files.
 * @param is_fold_case Whether ASCII case differences are ignored when matching a suffix.
 * @param reason       Receives the failure reason, always naming the exact path the failure
 *                     happened on: the directory that could not be inspected, opened, read or
 *                     closed, or the entry that could not be inspected. A caller must not append
 *                     the root it passed, because the reason is already more precise than that. May
 *                     be `NULL` only when `reason_len` is 0. Untouched on success.
 * @param reason_len   Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a directory, entry, or allocation failure.
 */
int fs_list_files_with_suffixes(struct PathList* paths,
                                const char* root_dir,
                                const char* excluded_dir,
                                const char* const* suffixes,
                                size_t suffix_count,
                                bool is_fold_case,
                                char* reason,
                                size_t reason_len) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Reads a regular file into a freshly allocated, `NUL`-terminated byte buffer.
 *
 * On success the caller owns `*data_out` and must `free` it. It writes both output parameters only
 * on success. It rejects non-regular files and files that change size during a read. It never
 * reports a partial copy as a successful read.
 *
 * It opens the path without blocking and checks the opened descriptor, so the file inspected is the
 * file read and a FIFO is rejected rather than waited on. It rejects a file larger than
 * `data_len_max` from that `fstat` size, before any read or allocation, so an oversize input never
 * becomes resident. Each caller passes the limit for the kind of file it reads.
 *
 * The buffer holds one extra `NUL` terminator, but its first `*data_len_out` bytes may contain
 * arbitrary data including `NUL`, because this also reads binary media. Text consumers establish
 * the codebase's `NUL`-free text invariant themselves by calling `text_is_nul_free`.
 *
 * @param file_path    Path of the file to read. Must not be `NULL`.
 * @param data_len_max Largest accepted file size in bytes, excluding the terminator this adds. Must
 *                     be less than `SIZE_MAX`, which leaves room for the terminator.
 * @param data_out     Receives the malloc'd buffer holding the file bytes plus a terminator. Must
 *                     not be `NULL`.
 * @param data_len_out Receives the number of bytes read, excluding the terminator. Must not be
 *                     `NULL`.
 * @param reason       Receives the failure reason, which tells the first-party rejections apart
 *                     from the system ones. May be `NULL` only when `reason_len` is 0. Untouched on
 *                     success.
 * @param reason_len   Size of `reason` in bytes.
 * @return `0` on success, or `-1` when the file is missing, not regular, larger than `data_len_max`
 *         or changed size mid-read, and on an open, read, allocation or close failure.
 */
int fs_read_file(const char* file_path,
                 size_t data_len_max,
                 unsigned char** data_out,
                 size_t* data_len_out,
                 char* reason,
                 size_t reason_len) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Writes `data_len` bytes to `file_path`, creating parent directories as needed.
 *
 * Overwrites any existing file in place. The write is deliberately neither atomic nor `fsync`ed, so
 * a reader can observe a partial file while a write is running and an interrupted build can leave
 * one behind. That is acceptable because an `output_dir` is reproducible: deleting it and building
 * again recovers a truncated file in one command, and recovers stale outputs and a half-finished
 * phase at the same time, which a temp-write-plus-rename protocol would not. Do not add such a
 * protocol without a caller that cannot recover by rebuilding. A caller that trusts an existing
 * output instead of regenerating it, such as a freshness check, is that case, because it would take
 * a truncated file for a finished one. The derivative phase makes that check and guards it by size
 * instead: it regenerates an empty output, and keeps a copied original only when its size matches
 * its source's. A derivative has no size to match before it is encoded, so one cut short after its
 * first byte is kept until its source changes or `output_dir` is deleted.
 *
 * A newly created file gets mode `0666` reduced by the process umask. An existing destination keeps
 * its own mode, because the write goes through the file already there.
 *
 * This and `fs_copy_file` are the only writers and should stay the only ones. A further writer
 * covering a subset of the outputs gives a caller a choice to get wrong. `data` is `const void*` so
 * a text and a binary caller both pass their buffer without a cast.
 *
 * @param file_path  Destination path. Must not be `NULL`.
 * @param data       Source bytes. Must hold at least `data_len` bytes. Must not be `NULL`.
 * @param data_len   Number of bytes to write.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 *                   Untouched on success.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a directory, open, write, or close failure.
 */
int fs_write_file(const char* file_path,
                  const void* data,
                  size_t data_len,
                  char* reason,
                  size_t reason_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Streams one regular file into another path, truncating the destination.
 *
 * The destination does not inherit the source's mode. It gets the same `0666`-minus-umask treatment
 * as every other file this module creates, so a published copy's permissions do not depend on the
 * source tree. Like `fs_write_file`, this overwrites in place and is neither atomic nor synced.
 *
 * @param source_path Regular file to read. Must not be `NULL`.
 * @param dest_path   Destination path, whose parent directories are created as needed. Must not be
 *                    `NULL`.
 * @param reason      Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 *                    Untouched on success.
 * @param reason_len  Size of `reason` in bytes.
 * @return `0` on success, or `-1` when the source is missing or not regular, and on a directory,
 *         open, read, write, or close failure.
 */
int fs_copy_file(const char* source_path, const char* dest_path, char* reason, size_t reason_len)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reads a regular file's modification timestamp and byte size.
 *
 * Symlinks are followed, so the metadata is the target's.
 *
 * @param file_path  Path of the file to inspect. Must not be `NULL`.
 * @param meta_out   Receives the timestamp and size. Written only on success. Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 *                   Untouched on success.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` when the file cannot be inspected, is not regular, or has a size
 *         `size_t` cannot represent.
 */
int fs_stat_meta(const char* file_path, struct FsMeta* meta_out, char* reason, size_t reason_len)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Creates `dir_path` and any missing parent directories.
 *
 * It accepts existing directories along the path rather than treating them as errors.
 *
 * @param dir_path   Directory path to create. An empty string is a no-op. Must not be `NULL`.
 * @param reason     Receives the failure reason, naming the component that could not be created.
 *                   May be `NULL` only when `reason_len` is 0. Untouched on success.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` if a component cannot be created.
 */
int fs_mkdir_p(const char* dir_path, char* reason, size_t reason_len) __attribute__((nonnull(1)));

/**
 * @brief Reports whether a path exists, following symlinks.
 *
 * Like `fs_identify`, this takes no `reason`, because its caller wants a fact rather than a
 * diagnostic. It answers only "is there something here", not "is it usable": a path that exists but
 * is the wrong type still reports `true`, so a caller that needs a directory hands the path to an
 * operation that reports the type mismatch itself. A dangling symlink reports `false`, because
 * `stat` follows the link and cannot tell a missing target from a missing path.
 *
 * This exists so an optional configured directory can be skipped rather than walked. The walk
 * requires its root to exist, so walking an absent optional root would fail for a directory that is
 * simply not there.
 *
 * @param path Path to inspect. Symlinks are followed, so existence is the target's. Must not be
 *             `NULL`.
 * @return `true` when something exists at `path`, or `false` when nothing does or it cannot be
 *         inspected.
 */
bool fs_path_exists(const char* path) __attribute__((nonnull(1)));

/**
 * @brief Requires a path to exist and be a directory, reporting why when it does not.
 *
 * This is the reporting counterpart to `fs_path_exists`. Use the predicate to decide whether an
 * optional directory is there, where absence is the expected answer and no diagnostic is wanted,
 * and use this when a directory is required and the caller has to tell the user what is wrong. A
 * predicate cannot do the second job: `ENOENT` and `EACCES` call for different fixes, and a caller
 * that returns on `false` gives no later operation the chance to report the cause.
 *
 * The reason is the bare cause with the path trailing it, so a caller composes it after naming its
 * own operation and subject, as in
 * `failed to resolve config directory 'input_dir': No such file or directory ('photos')`.
 *
 * @param dir_path   Path that must be an existing directory. Symlinks are followed. Must not be
 *                   `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 *                   Untouched on success.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` when `dir_path` is an existing directory, or `-1` when it cannot be inspected or is
 *         not a directory.
 */
int fs_require_dir(const char* dir_path, char* reason, size_t reason_len)
    __attribute__((nonnull(1)));

/**
 * @brief Reports the identity of the file at `file_path`, when it exists and can be inspected.
 *
 * Unlike the other actions here this one takes no `reason`, because its caller wants a fact rather
 * than a diagnostic. "no identity" and "not the same file" lead to the same decision. A path that
 * does not exist has no identity and is not a failure worth reporting. It is the ordinary case for
 * an output path about to be created. A path that exists but cannot be inspected also reports `-1`,
 * which is safe here for the same reason. A caller that cannot `stat` a path is not going to write
 * it either. The write reports that failure with its own system message.
 *
 * @param file_path    Path to inspect. Symlinks are followed, so the identity is the target's. Must
 *                     not be `NULL`.
 * @param identity_out Receives the device and inode pair. Written only on success. Must not be
 *                     `NULL`.
 * @return `0` when `identity_out` was filled, or `-1` when `file_path` could not be inspected.
 */
int fs_identify(const char* file_path, struct FsIdentity* identity_out)
    __attribute__((nonnull(1, 2)));

#endif
