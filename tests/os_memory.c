// `TEST_NO_MAIN` lets the fixture helpers here record failures with `TEST_CHECK`, as in
// `test_support.c`.
#define TEST_NO_MAIN

// `PATH_MAX` is outside strict C17, so `<limits.h>` hides it unless a feature-test macro asks for
// it, as in `fs.c`: `_DARWIN_C_SOURCE` on macOS, `_DEFAULT_SOURCE` on glibc and musl. Both must
// precede every `#include`.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "os_memory.h"

#include <acutest.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/text.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * Symlink expansions one lookup follows before it fails with `ELOOP`, as the kernel the tests run
 * on does: 32 on macOS and 40 on Linux. The C libraries on Linux define `MAXSYMLINKS` as 20, below
 * what the kernel follows, so the number is spelled out for each.
 */
#ifdef __APPLE__
enum { OS_MEMORY_LINK_EXPANSION_COUNT_MAX = 32 };
#else
enum { OS_MEMORY_LINK_EXPANSION_COUNT_MAX = 40 };
#endif

/** What one node of the tree is. */
enum OsMemoryNodeKind {
  /** A directory, whose entries are the `OsMemoryEntry` items naming it as their `dir`. */
  OS_MEMORY_NODE_DIR,

  /** A regular file. */
  OS_MEMORY_NODE_FILE,

  /** A symlink. */
  OS_MEMORY_NODE_LINK,
};

/** One file, directory or symlink. Its index in `Os.nodes` is its inode. */
struct OsMemoryNode {
  /** What the node is. */
  enum OsMemoryNodeKind kind;

  /** A file's contents, or a symlink's target. */
  struct Str data;

  /**
   * Bytes past `data` that a file holds and that read as `NUL`, from `os_memory_truncate`. A write
   * replaces them along with `data`.
   */
  ptrdiff_t zero_len;

  /** A directory's parent directory, which `..` names. The root is its own parent. */
  ptrdiff_t parent;

  /** Value of `Os.clock` when the node was last created, written or touched. */
  int64_t mtime;
};

/** One name in one directory. Two entries naming one file node are hard links. */
struct OsMemoryEntry {
  /** Directory node holding the entry. */
  ptrdiff_t dir;

  /** Bare name, never `.` or `..`. */
  struct Str name;

  /** Node the name refers to. */
  ptrdiff_t node;
};

/** One injected failure. */
struct OsMemoryFault {
  /** Operation that fails. */
  enum OsMemoryOp op;

  /** Path the failure applies to, and every path below it. */
  struct Str path;

  /** `errno` value the operation fails with. */
  int error_number;
};

/** The in-memory host. */
struct Os {
  /** Held by every operation, so parallel jobs can share the host. */
  pthread_mutex_t lock;

  /** Arena that owns the tree and the printed text. Nothing is freed. */
  struct Arena arena;

  /** Region each operation copies by value for its temporaries, such as expanded link paths. */
  struct Arena scratch;

  /**
   * Region each fixture writer copies by value for the log of the operation it runs, which nothing
   * else allocates from, so a test that writes a fixture for each generated case reuses one
   * reservation. The fixture writers run on the test's own thread, never during a parallel phase.
   */
  struct Arena fixture_log;

  /** Array of `node_capacity` nodes. Node 0 is the root directory. */
  struct OsMemoryNode* nodes;

  /** Number of populated entries in `nodes`. */
  ptrdiff_t node_count;

  /** Allocated slots in `nodes`. */
  ptrdiff_t node_capacity;

  /** Array of `entry_capacity` directory entries. */
  struct OsMemoryEntry* entries;

  /** Number of populated entries in `entries`. */
  ptrdiff_t entry_count;

  /** Allocated slots in `entries`. */
  ptrdiff_t entry_capacity;

  /** Array of `fault_capacity` injected failures. */
  struct OsMemoryFault* faults;

  /** Number of populated entries in `faults`. */
  ptrdiff_t fault_count;

  /** Allocated slots in `faults`. */
  ptrdiff_t fault_capacity;

  /** Modification time of the most recent change, which every change advances by one. */
  int64_t clock;

  /** Text `os_print` wrote to standard output and standard error. */
  struct Buf printed[2];
};

/** Where a lookup ended. */
struct OsMemoryLookup {
  /** Node the path names, or `-1` when it names nothing. */
  ptrdiff_t node;

  /**
   * Directory that holds, or would hold, the last component, or `-1` when a component before the
   * last is missing.
   */
  ptrdiff_t dir;

  /** Last component, after every symlink the lookup followed. */
  struct Str name;
};

/**
 * Where an operation reports its failure, as in the POSIX implementation: the caller's log and
 * operation.
 */
struct OsMemoryReport {
  /** Operation the caller was performing. */
  struct Str operation;

  /** Log that receives the message. */
  struct Log* log;
};

/** What `os_list_classify` found one directory entry to be, as in the POSIX implementation. */
enum ListClassify {
  /** A regular file or a directory, once symlinks are followed, so the listing keeps it. */
  LIST_CLASSIFY_LISTED,

  /** A symlink that resolves to nothing, so the listing leaves it out. */
  LIST_CLASSIFY_SKIPPED,

  /** An entry that exists but could not be inspected, with `errno` set, so the listing fails. */
  LIST_CLASSIFY_FAILED,
};

/**
 * @brief Creates a directory and any missing parents as a fixture, recording a test failure when
 *        one cannot be created.
 *
 * @param os       Host to create the directories on. Must not be `NULL`.
 * @param dir_path Directory path to create. An empty string is a no-op.
 */
static void fixture_mkdirs(struct Os* os, struct Str dir_path) __attribute__((nonnull(1)));

/**
 * @brief Appends each regular file below directory `dir`, without following symlinks. A step of
 *        `os_memory_files`.
 *
 * @param os       Host to walk. Must not be `NULL`.
 * @param paths    List that receives the paths. Must not be `NULL`.
 * @param dir      Directory node to walk.
 * @param dir_path Path that names `dir`, which each listed path starts with.
 */
static void os_memory_files_below(struct Os* os,
                                  struct PathList* paths,
                                  ptrdiff_t dir,
                                  struct Str dir_path) __attribute__((nonnull(1, 2)));

/**
 * @brief Compares two `PathList` items for `qsort`.
 *
 * @param a Pointer to the first `struct Str` element. Must not be `NULL`.
 * @param b Pointer to the second `struct Str` element. Must not be `NULL`.
 * @return A negative, zero, or positive value per `str_compare` of the two paths.
 */
static int compare_paths(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Takes the host's lock, ending the process if it cannot. Worker threads call this, and
 *        acutest's checks are not safe to call from them.
 *
 * @param os Host to lock. Must not be `NULL`.
 */
static void host_lock(struct Os* os) __attribute__((nonnull(1)));

/**
 * @brief Releases the host's lock, preserving `errno` for a caller that reports it.
 *
 * @param os Host to unlock. Must not be `NULL`.
 */
static void host_unlock(struct Os* os) __attribute__((nonnull(1)));

/**
 * @brief Looks a path up, following symlinks on every component and optionally on the last one.
 *
 * @param os         Host to search. Must not be `NULL`.
 * @param path       Path to look up, relative to the root whether or not it starts with `/`.
 * @param is_follow  Whether a symlink in the last component is followed.
 * @param arena      Arena for expanded link paths, which `lookup_out->name` may point into. Must
 *                   not be `NULL`.
 * @param lookup_out Receives where the lookup ended, on failure too. Must not be `NULL`.
 * @return `0` when the path names a node, or `-1` with `errno` set to `ENOENT`, `ENOTDIR` or
 *         `ELOOP`.
 */
static int tree_lookup(struct Os* os,
                       struct Str path,
                       bool is_follow,
                       struct Arena* arena,
                       struct OsMemoryLookup* lookup_out) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Adds a node and an entry naming it.
 *
 * @param os   Host to extend. Must not be `NULL`.
 * @param dir  Directory that receives the entry.
 * @param name Name of the entry, copied into the host.
 * @param kind What the new node is.
 * @param data Contents or target, copied into the host.
 */
static void node_add(struct Os* os,
                     ptrdiff_t dir,
                     struct Str name,
                     enum OsMemoryNodeKind kind,
                     struct Str data) __attribute__((nonnull(1)));

/**
 * @brief Adds a node that no entry names yet.
 *
 * @param os     Host to extend. Must not be `NULL`.
 * @param kind   What the new node is.
 * @param data   Contents or target, which the host's arena already owns.
 * @param parent Directory that holds, or will hold, the node.
 * @return The new node's index.
 */
static ptrdiff_t node_new(struct Os* os,
                          enum OsMemoryNodeKind kind,
                          struct Str data,
                          ptrdiff_t parent) __attribute__((nonnull(1)));

/**
 * @brief Returns the entry naming `name` in directory `dir`, or `NULL`.
 *
 * @param os   Host to search. Must not be `NULL`.
 * @param dir  Directory node.
 * @param name Bare name.
 * @return The entry, which a caller holding the lock may point at another node, or `NULL` when
 *         `dir` holds no such name.
 */
static struct OsMemoryEntry* entry_find(struct Os* os, ptrdiff_t dir, struct Str name)
    __attribute__((nonnull(1)));

/**
 * @brief Adds an entry naming an existing node.
 *
 * @param os   Host to extend. Must not be `NULL`.
 * @param dir  Directory that receives the entry.
 * @param name Name of the entry, copied into the host.
 * @param node Node the entry names.
 */
static void entry_add(struct Os* os, ptrdiff_t dir, struct Str name, ptrdiff_t node)
    __attribute__((nonnull(1)));

/**
 * @brief Returns the entry a lookup ended on, or `NULL` when it ended on a directory itself, as a
 *        path naming the root or ending in `.` or `..` does.
 * @param os     Host searched. Must not be `NULL`.
 * @param lookup Successful lookup. Must not be `NULL`.
 * @return The entry naming `lookup->node`, or `NULL`.
 */
static const struct OsMemoryEntry* entry_of_lookup(struct Os* os,
                                                   const struct OsMemoryLookup* lookup)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Removes one entry, keeping the others in their order.
 * @param os    Host to change. Must not be `NULL`.
 * @param entry Entry of `os` to remove. Must not be `NULL`.
 */
static void entry_remove(struct Os* os, const struct OsMemoryEntry* entry)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the `errno` value an injected failure gives `op` on `path`, or `0` for none.
 *
 * @param os   Host to look in. Must not be `NULL`.
 * @param op   Operation the caller performs.
 * @param path Path the caller passed.
 * @return The injected `errno` value, or `0`.
 */
static int fault_find(const struct Os* os, enum OsMemoryOp op, struct Str path)
    __attribute__((nonnull(1)));

/**
 * @brief Finds the regular file `os_read` reads, or an `OS_SOURCE_FILE` write copies, as the POSIX
 *        implementation opens it. The caller holds the lock.
 *
 * @param os        Host to search. Must not be `NULL`.
 * @param file_path Path of the file, which an injected `OS_MEMORY_OP_READ` failure matches.
 * @param node_out  Receives the file's node. Written only on success. Must not be `NULL`.
 * @param report    Where a failure is reported, naming `file_path`. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file is missing or not regular.
 */
static int open_regular(struct Os* os,
                        struct Str file_path,
                        ptrdiff_t* node_out,
                        const struct OsMemoryReport* report) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Writes a file and its parents. The body of `os_write`. The caller holds the lock.
 *
 * @param os        Host to write to. Must not be `NULL`.
 * @param file_path Destination path.
 * @param source    Bytes to write, copied into the host.
 * @param report    Where a failure is reported. Must not be `NULL`.
 * @return `0` on success, or `-1` on failure.
 */
static int os_write_locked(struct Os* os,
                           struct Str file_path,
                           struct OsSource source,
                           const struct OsMemoryReport* report) __attribute__((nonnull(1, 4)));

/**
 * @brief Looks up the file an `os_write_locked` call writes, as the POSIX `os_write_open` opens it.
 *        A step of `os_write_locked`.
 *
 * The write goes through a symlink at the path, as `open` does.
 *
 * @param os           Host to search. Must not be `NULL`.
 * @param file_path    Destination path, which an injected `OS_MEMORY_OP_WRITE` failure matches.
 * @param arena        Arena for expanded link paths, which `lookup_out->name` may point into. Must
 *                     not be `NULL`.
 * @param lookup_out   Receives where the destination's lookup ended. Written only on success. Must
 *                     not be `NULL`.
 * @param is_found_out Receives whether the lookup found an entry. Written only on success. Must not
 *                     be `NULL`.
 * @param report       Where a failure is reported. Must not be `NULL`.
 * @return `0` when the destination exists and is not a directory, or is missing from a directory
 *         that exists, or `-1` otherwise.
 */
static int os_write_locked_open(struct Os* os,
                                struct Str file_path,
                                struct Arena* arena,
                                struct OsMemoryLookup* lookup_out,
                                bool* is_found_out,
                                const struct OsMemoryReport* report)
    __attribute__((nonnull(1, 3, 4, 5, 6)));

/**
 * @brief Collects the new contents of a file from its source. A step of `os_write_locked`.
 *
 * @param os           Host the contents go in. Must not be `NULL`.
 * @param file_path    Destination path, which an injected `OS_MEMORY_OP_WRITE_DATA` failure
 *                     matches.
 * @param source       Bytes to write.
 * @param source_node  Node of the file an `OS_SOURCE_FILE` write copies, or `-1`.
 * @param data_out     Receives the contents, which the host's arena owns, on failure too. Must not
 *                     be `NULL`.
 * @param zero_len_out Receives the count of `NUL` bytes past `data_out`. Must not be `NULL`.
 * @param report       Where a failure is reported. Must not be `NULL`.
 * @return `0` on success, or `-1` on an injected failure, when `data_out` is empty.
 */
static int os_write_locked_data(struct Os* os,
                                struct Str file_path,
                                struct OsSource source,
                                ptrdiff_t source_node,
                                struct Str* data_out,
                                ptrdiff_t* zero_len_out,
                                const struct OsMemoryReport* report)
    __attribute__((nonnull(1, 5, 6, 7)));

/**
 * @brief Stores a file's new contents at the destination a lookup ended at. A step of
 *        `os_write_locked`.
 *
 * A write to an existing file changes that file's node. Otherwise a new file node takes a new entry
 * where the lookup ended.
 *
 * @param os       Host to change. Must not be `NULL`.
 * @param lookup   Where the destination's lookup ended. Must not be `NULL`.
 * @param is_found Whether the lookup found an entry.
 * @param data     Contents of the file, which the host's arena owns.
 * @param zero_len Count of `NUL` bytes past `data`.
 */
static void os_write_locked_store(struct Os* os,
                                  const struct OsMemoryLookup* lookup,
                                  bool is_found,
                                  struct Str data,
                                  ptrdiff_t zero_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Creates `dir_path` and any missing parents. The body of `os_mkdirs`, which `os_write`
 *        shares for a file's parent directories. The caller holds the lock.
 *
 * @param os       Host to create the directories on. Must not be `NULL`.
 * @param dir_path Directory path to create. An empty string is a no-op.
 * @param report   Where a failure is reported. Must not be `NULL`.
 * @return `0` on success, or `-1` if a component cannot be created.
 */
static int mkdirs_report(struct Os* os, struct Str dir_path, const struct OsMemoryReport* report)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Creates one directory, accepting a path that already names one, as `mkdir` and `stat` do.
 *
 * @param os       Host to create the directory on. Must not be `NULL`.
 * @param dir_path Directory path to create.
 * @param report   Where a failure is reported. Must not be `NULL`.
 * @return `0` on success or when `dir_path` already names a directory, or `-1` on failure.
 */
static int mkdirs_report_one(struct Os* os,
                             struct Str dir_path,
                             const struct OsMemoryReport* report) __attribute__((nonnull(1, 3)));

/**
 * @brief Classifies one directory entry, following it when it is a symlink. A step of `os_list`.
 *
 * @param os         Host holding the entry. Must not be `NULL`.
 * @param entry      Entry to classify. Must not be `NULL`.
 * @param entry_path Path of the entry, which an injected `OS_MEMORY_OP_STAT` failure matches.
 * @param scratch    Arena for the expanded link paths of a symlink's lookup.
 * @param kind_out   Receives what the entry names. Must not be `NULL`.
 * @return `LIST_CLASSIFY_LISTED` when the entry names a regular file or a directory and the outputs
 *         are written, `LIST_CLASSIFY_SKIPPED` for a symlink that resolves to nothing, or
 *         `LIST_CLASSIFY_FAILED` with `errno` set when it exists but could not be inspected.
 */
static enum ListClassify os_list_classify(struct Os* os,
                                          const struct OsMemoryEntry* entry,
                                          struct Str entry_path,
                                          struct Arena scratch,
                                          enum OsEntryKind* kind_out)
    __attribute__((nonnull(1, 2, 5)));

/**
 * @brief Reports `error_number` as the cause of the caller's operation failing on `path`, in the
 *        words of the POSIX implementation.
 *
 * @param report       Where the failure is reported. Must not be `NULL`.
 * @param path         Path the operation targeted.
 * @param error_number `errno` value to describe.
 * @return `-1` always.
 */
static int report_errno(const struct OsMemoryReport* report, struct Str path, int error_number)
    __attribute__((nonnull(1)));

/**
 * @brief Reports `error_number` as the cause of one step of the caller's operation failing.
 *
 * @param report       Where the failure is reported. Must not be `NULL`.
 * @param step         Lowercase step that failed, such as `open directory`.
 * @param path         Path the step was attempted on.
 * @param error_number `errno` value to describe.
 * @return `-1` always.
 */
static int report_step_errno(const struct OsMemoryReport* report,
                             struct Str step,
                             struct Str path,
                             int error_number) __attribute__((nonnull(1)));

struct Os* os_memory(void) {
  struct Arena arena = test_arena();
  struct Os* os = arena_new(&arena, 1, struct Os);
  os->arena = arena;
  os->scratch = test_arena();
  os->fixture_log = test_arena();
  if (pthread_mutex_init(&os->lock, NULL) != 0) {
    abort();
  }
  for (ptrdiff_t i = 0; i < 2; i++) {
    os->printed[i].arena = &os->arena;
  }
  os->nodes = arena_grow(&os->arena, os->nodes, &os->node_capacity, sizeof(*os->nodes),
                         _Alignof(struct OsMemoryNode));
  os->nodes[0] = (struct OsMemoryNode){.kind = OS_MEMORY_NODE_DIR, .parent = 0};
  os->node_count = 1;
  return os;
}

void os_memory_write(struct Os* os, const char* file_path, const char* contents) {
  os_memory_write_bytes(os, str_from_cstr(file_path), str_from_cstr(contents));
}

void os_memory_write_bytes(struct Os* os, struct Str file_path, struct Str contents) {
  struct Arena log_arena = os->fixture_log;
  struct Log log = {.buf = {.arena = &log_arena}};
  TEST_CHECK(os_write(os, file_path, (struct OsSource){.bytes = contents},
                      S("failed to write fixture"), &log) == 0);
  const struct Str failure = buf_str(&log.buf);
  TEST_MSG("%.*s", STR_FMT(failure));
}

void os_memory_truncate(struct Os* os, const char* file_path, ptrdiff_t len) {
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup;
  const bool is_file = tree_lookup(os, str_from_cstr(file_path), true, &scratch, &lookup) == 0 &&
                       os->nodes[lookup.node].kind == OS_MEMORY_NODE_FILE;
  struct OsMemoryNode* node = is_file ? &os->nodes[lookup.node] : NULL;
  if (TEST_CHECK_(node != NULL && len >= node->data.len + node->zero_len,
                  "'%s' is a file no longer than %td bytes", file_path, len)) {
    node->zero_len = len - node->data.len;
  }
  host_unlock(os);
}

void os_memory_touch(struct Os* os, const char* file_path) {
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup;
  const bool is_found = tree_lookup(os, str_from_cstr(file_path), true, &scratch, &lookup) == 0;
  if (TEST_CHECK_(is_found, "'%s' exists", file_path)) {
    os->nodes[lookup.node].mtime = ++os->clock;
  }
  host_unlock(os);
}

void os_memory_mkdir(struct Os* os, const char* dir_path) {
  fixture_mkdirs(os, str_from_cstr(dir_path));
}

void os_memory_symlink(struct Os* os, const char* target, const char* link_path) {
  fixture_mkdirs(os, path_parent(str_from_cstr(link_path)));
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup;
  const bool is_free = tree_lookup(os, str_from_cstr(link_path), false, &scratch, &lookup) != 0 &&
                       errno == ENOENT && lookup.dir >= 0;
  if (TEST_CHECK_(is_free, "symlink path '%s' is free", link_path)) {
    node_add(os, lookup.dir, lookup.name, OS_MEMORY_NODE_LINK, str_from_cstr(target));
  }
  host_unlock(os);
}

void os_memory_link(struct Os* os, const char* file_path, const char* link_path) {
  fixture_mkdirs(os, path_parent(str_from_cstr(link_path)));
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup file;
  struct OsMemoryLookup link;
  const bool is_file = tree_lookup(os, str_from_cstr(file_path), true, &scratch, &file) == 0 &&
                       os->nodes[file.node].kind == OS_MEMORY_NODE_FILE;
  const bool is_free = tree_lookup(os, str_from_cstr(link_path), false, &scratch, &link) != 0 &&
                       errno == ENOENT && link.dir >= 0;
  if (TEST_CHECK_(is_file && is_free, "hard link '%s' to '%s' can be created", link_path,
                  file_path)) {
    entry_add(os, link.dir, link.name, file.node);
  }
  host_unlock(os);
}

void os_memory_fail(struct Os* os, enum OsMemoryOp op, const char* path, int error_number) {
  // A fault on `x/` would match the paths below `x` but never `x` itself, which no caller means.
  TEST_ASSERT_(!str_endswith(str_from_cstr(path), S("/")), "fault path '%s' ends in '/'", path);
  host_lock(os);
  if (error_number == 0) {
    ptrdiff_t kept = 0;
    for (ptrdiff_t i = 0; i < os->fault_count; i++) {
      const struct OsMemoryFault* fault = &os->faults[i];
      if (fault->op != op || !str_eq(fault->path, str_from_cstr(path))) {
        os->faults[kept++] = *fault;
      }
    }
    os->fault_count = kept;
    host_unlock(os);
    return;
  }
  if (os->fault_count == os->fault_capacity) {
    os->faults = arena_grow(&os->arena, os->faults, &os->fault_capacity, sizeof(*os->faults),
                            _Alignof(struct OsMemoryFault));
  }
  os->faults[os->fault_count++] =
      (struct OsMemoryFault){op, str_clone(str_from_cstr(path), &os->arena), error_number};
  host_unlock(os);
}

bool os_memory_is_file(struct Os* os, const char* file_path, const char* contents) {
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup;
  const bool is_file = tree_lookup(os, str_from_cstr(file_path), true, &scratch, &lookup) == 0 &&
                       os->nodes[lookup.node].kind == OS_MEMORY_NODE_FILE;
  const struct OsMemoryNode node = is_file ? os->nodes[lookup.node] : (struct OsMemoryNode){0};
  host_unlock(os);
  // A file extended by `os_memory_truncate` holds bytes `contents` cannot spell, so it never
  // matches.
  const bool is_equal = is_file && node.zero_len == 0 && str_eq(node.data, str_from_cstr(contents));
  TEST_CHECK_(is_equal, "'%s' holds the expected text", file_path);
  if (!is_file) {
    TEST_MSG("no regular file at '%s'", file_path);
  } else if (node.zero_len > 0) {
    TEST_MSG("'%s' holds %td bytes past its contents", file_path, node.zero_len);
  } else {
    TEST_MSG("expected: '%s'", contents);
    TEST_MSG("actual:   '%.*s'", STR_FMT(node.data));
  }
  return is_equal;
}

bool os_memory_has_path(struct Os* os, const char* path) {
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup;
  const bool exists = tree_lookup(os, str_from_cstr(path), false, &scratch, &lookup) == 0;
  host_unlock(os);
  return exists;
}

int os_memory_file(struct Os* os, const char* file_path, struct Str* data_out) {
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup;
  // A file extended by `os_memory_truncate` holds bytes no test spells out, so it never matches.
  const bool is_file = tree_lookup(os, str_from_cstr(file_path), true, &scratch, &lookup) == 0 &&
                       os->nodes[lookup.node].kind == OS_MEMORY_NODE_FILE &&
                       os->nodes[lookup.node].zero_len == 0;
  if (is_file) {
    *data_out = os->nodes[lookup.node].data;
  }
  host_unlock(os);
  return is_file ? 0 : -1;
}

struct PathList os_memory_files(struct Os* os, const char* dir_path) {
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup;
  struct PathList paths = {0};
  const struct Str path = str_from_cstr(dir_path);
  if (tree_lookup(os, path, true, &scratch, &lookup) == 0 &&
      os->nodes[lookup.node].kind == OS_MEMORY_NODE_DIR) {
    os_memory_files_below(os, &paths, lookup.node, path);
  }
  if (paths.count > 0) {
    qsort(paths.items, (size_t)paths.count, sizeof(*paths.items), compare_paths);
  }
  host_unlock(os);
  return paths;
}

struct Str os_memory_printed(struct Os* os, int fd) {
  assert(fd == 1 || fd == 2);
  host_lock(os);
  const struct Str text = buf_str(&os->printed[fd == 1 ? 0 : 1]);
  host_unlock(os);
  return text;
}

bool os_memory_is_printed(struct Os* os, int fd, const char* expected) {
  const struct Str text = os_memory_printed(os, fd);
  const bool is_equal = str_eq(text, str_from_cstr(expected));
  TEST_CHECK_(is_equal, "standard %s holds the expected text", fd == 1 ? "output" : "error");
  TEST_MSG("expected: '%s'", expected);
  TEST_MSG("actual:   '%.*s'", STR_FMT(text));
  return is_equal;
}

int os_read(struct Os* os,
            struct Str file_path,
            ptrdiff_t data_len_max,
            struct Arena* perm,
            struct Str* data_out,
            struct Str operation,
            struct Log* log) {
  const struct OsMemoryReport report = {operation, log};
  host_lock(os);
  ptrdiff_t file_node = -1;
  int rc = open_regular(os, file_path, &file_node, &report);
  if (rc == 0) {
    const struct OsMemoryNode* node = &os->nodes[file_node];
    const struct Str data = node->data;
    const ptrdiff_t len = data.len + node->zero_len;
    if (len > data_len_max) {
      rc = log_error(log, "%.*s: exceeds max file size (%td bytes) at %td bytes ('%.*s')",
                     STR_FMT(operation), data_len_max, len, STR_FMT(file_path));
    } else {
      // The arena zeroes what it hands out, so the bytes past `data` read as `NUL`, as the bytes
      // past the old end of a truncated file do.
      char* bytes = arena_new(perm, len, char);
      if (data.len > 0) {
        memcpy(bytes, data.data, (size_t)data.len);
      }
      *data_out = (struct Str){bytes, len};
    }
  }
  host_unlock(os);
  return rc;
}

int os_write(struct Os* os,
             struct Str file_path,
             struct OsSource source,
             struct Str operation,
             struct Log* log) {
  const struct OsMemoryReport report = {operation, log};
  host_lock(os);
  const int rc = os_write_locked(os, file_path, source, &report);
  host_unlock(os);
  return rc;
}

int os_mkdirs(struct Os* os, struct Str dir_path, struct Str operation, struct Log* log) {
  const struct OsMemoryReport report = {operation, log};
  host_lock(os);
  const int rc = mkdirs_report(os, dir_path, &report);
  host_unlock(os);
  return rc;
}

int os_remove(struct Os* os, struct Str file_path, struct Str operation, struct Log* log) {
  const struct OsMemoryReport report = {operation, log};
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup = {0};
  int rc = 0;
  const int fault = fault_find(os, OS_MEMORY_OP_REMOVE, file_path);
  if (fault != 0) {
    rc = report_errno(&report, file_path, fault);
  } else if (tree_lookup(os, file_path, false, &scratch, &lookup) != 0) {
    // A path that names nothing is already removed, as `unlink` failing with `ENOENT` is.
    rc = errno == ENOENT ? 0 : report_errno(&report, file_path, errno);
  } else if (os->nodes[lookup.node].kind == OS_MEMORY_NODE_DIR) {
    // `unlink` refuses a directory with `EPERM` on macOS and `EISDIR` on Linux.
#ifdef __APPLE__
    rc = report_errno(&report, file_path, EPERM);
#else
    rc = report_errno(&report, file_path, EISDIR);
#endif
  } else {
    entry_remove(os, entry_of_lookup(os, &lookup));
  }
  host_unlock(os);
  return rc;
}

int os_remove_dir(struct Os* os, struct Str dir_path, struct Str operation, struct Log* log) {
  const struct OsMemoryReport report = {operation, log};
  // A last component followed by `/` is looked up through a symlink, so the trailing `/` characters
  // go, as the POSIX host drops them, and a symlink fails as `ENOTDIR` with or without them. A path
  // the system could not take keeps them, so the lookup fails it as `ENAMETOOLONG`, as POSIX does.
  struct Str lookup_path = dir_path;
  while (dir_path.len < PATH_MAX && lookup_path.len > 1 &&
         lookup_path.data[lookup_path.len - 1] == '/') {
    lookup_path = str_head(lookup_path, lookup_path.len - 1);
  }
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup = {0};
  int rc = 0;
  const int fault = fault_find(os, OS_MEMORY_OP_REMOVE, dir_path);
  if (fault != 0) {
    rc = report_errno(&report, dir_path, fault);
  } else if (tree_lookup(os, lookup_path, false, &scratch, &lookup) != 0) {
    rc = errno == ENOENT ? 0 : report_errno(&report, dir_path, errno);
  } else if (os->nodes[lookup.node].kind != OS_MEMORY_NODE_DIR) {
    rc = report_errno(&report, dir_path, ENOTDIR);
  } else if (entry_of_lookup(os, &lookup) == NULL) {
    // `rmdir` refuses a path ending in `.` with `EINVAL`, and the root is not removable either.
    rc = report_errno(&report, dir_path, EINVAL);
  } else {
    bool is_empty = true;
    for (ptrdiff_t i = 0; is_empty && i < os->entry_count; i++) {
      is_empty = os->entries[i].dir != lookup.node;
    }
    if (is_empty) {
      entry_remove(os, entry_of_lookup(os, &lookup));
    } else {
      rc = report_errno(&report, dir_path, ENOTEMPTY);
    }
  }
  host_unlock(os);
  return rc;
}

int os_list(struct Os* os,
            struct Str dir_path,
            struct Arena* perm,
            struct OsEntry** entries_out,
            ptrdiff_t* count_out,
            struct Str operation,
            struct Log* log) {
  const struct OsMemoryReport report = {operation, log};
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup = {0};
  int rc = 0;
  const int fault = fault_find(os, OS_MEMORY_OP_LIST, dir_path);
  if (fault != 0) {
    rc = report_step_errno(&report, S("open directory"), dir_path, fault);
  } else if (tree_lookup(os, dir_path, true, &scratch, &lookup) != 0) {
    rc = report_step_errno(&report, S("open directory"), dir_path, errno);
  } else if (os->nodes[lookup.node].kind != OS_MEMORY_NODE_DIR) {
    rc = report_step_errno(&report, S("open directory"), dir_path, ENOTDIR);
  }

  // The array holds the directory's own entries, so a walk costs scratch in proportion to the tree
  // rather than to its directories times its entries.
  ptrdiff_t dir_entry_count = 0;
  for (ptrdiff_t i = 0; rc == 0 && i < os->entry_count; i++) {
    dir_entry_count += os->entries[i].dir == lookup.node;
  }
  struct Arena arena = *perm;
  struct OsEntry* entries = arena_new(&arena, dir_entry_count, struct OsEntry);
  ptrdiff_t count = 0;
  // The entries come newest first, an order unlike the one a test builds its tree in, so a caller
  // that relies on the order of a listing fails here as well as on a real directory.
  for (ptrdiff_t i = os->entry_count - 1; rc == 0 && i >= 0; i--) {
    const struct OsMemoryEntry* entry = &os->entries[i];
    if (entry->dir != lookup.node) {
      continue;
    }
    struct Arena join_arena = scratch;
    const struct Str entry_path = path_join(dir_path, entry->name, &join_arena);
    enum OsEntryKind kind = OS_ENTRY_FILE;
    const enum ListClassify classified = os_list_classify(os, entry, entry_path, join_arena, &kind);
    if (classified == LIST_CLASSIFY_FAILED) {
      entries[count] = (struct OsEntry){str_clone(entry->name, &arena), OS_ENTRY_FAILED, errno};
      count++;
    } else if (classified == LIST_CLASSIFY_LISTED) {
      entries[count] = (struct OsEntry){str_clone(entry->name, &arena), kind, 0};
      count++;
    }
  }
  if (rc == 0) {
    *perm = arena;
    *entries_out = entries;
    *count_out = count;
  }
  host_unlock(os);
  return rc;
}

int os_stat(struct Os* os, struct Str file_path, struct OsStat* stat_out) {
  host_lock(os);
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup;
  int rc = 0;
  const int fault = fault_find(os, OS_MEMORY_OP_STAT, file_path);
  if (fault != 0) {
    errno = fault;
    rc = -1;
  } else if (tree_lookup(os, file_path, true, &scratch, &lookup) != 0) {
    rc = -1;
  } else {
    const struct OsMemoryNode* node = &os->nodes[lookup.node];
    stat_out->identity = (struct OsIdentity){1, (uint64_t)lookup.node + 1};
    stat_out->is_dir = node->kind == OS_MEMORY_NODE_DIR;
    stat_out->len = node->kind == OS_MEMORY_NODE_DIR ? 0 : node->data.len + node->zero_len;
    stat_out->mtime_s = node->mtime;
    stat_out->mtime_ns = 0;
  }
  host_unlock(os);
  return rc;
}

int os_print(struct Os* os, int fd, struct Str text) {
  assert(fd == 1 || fd == 2);
  host_lock(os);
  buf_append(&os->printed[fd == 1 ? 0 : 1], text);
  host_unlock(os);
  return 0;
}

static void fixture_mkdirs(struct Os* os, struct Str dir_path) {
  struct Arena log_arena = os->fixture_log;
  struct Log log = {.buf = {.arena = &log_arena}};
  TEST_CHECK(os_mkdirs(os, dir_path, S("failed to create fixture"), &log) == 0);
  const struct Str failure = buf_str(&log.buf);
  TEST_MSG("%.*s", STR_FMT(failure));
}

static void os_memory_files_below(struct Os* os,
                                  struct PathList* paths,
                                  ptrdiff_t dir,
                                  struct Str dir_path) {
  for (ptrdiff_t i = 0; i < os->entry_count; i++) {
    const struct OsMemoryEntry* entry = &os->entries[i];
    if (entry->dir != dir) {
      continue;
    }
    const struct Str entry_path = path_join(dir_path, entry->name, &os->arena);
    switch (os->nodes[entry->node].kind) {
      case OS_MEMORY_NODE_DIR:
        os_memory_files_below(os, paths, entry->node, entry_path);
        break;
      case OS_MEMORY_NODE_FILE:
        path_list_push(paths, entry_path, &os->arena);
        break;
      case OS_MEMORY_NODE_LINK:
        break;
    }
  }
}

static int compare_paths(const void* a, const void* b) {
  return str_compare(*(const struct Str*)a, *(const struct Str*)b);
}

static void host_lock(struct Os* os) {
  if (pthread_mutex_lock(&os->lock) != 0) {
    abort();
  }
}

static void host_unlock(struct Os* os) {
  const int caller_errno = errno;
  if (pthread_mutex_unlock(&os->lock) != 0) {
    abort();
  }
  errno = caller_errno;
}

static int tree_lookup(struct Os* os,
                       struct Str path,
                       bool is_follow,
                       struct Arena* arena,
                       struct OsMemoryLookup* lookup_out) {
  *lookup_out = (struct OsMemoryLookup){.node = -1, .dir = -1};
  if (path.len == 0) {
    // The system names nothing with an empty path.
    errno = ENOENT;
    return -1;
  }
  // The POSIX implementation refuses these two before the system sees the path.
  if (path.len >= PATH_MAX) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (!text_is_nul_free(path)) {
    errno = EINVAL;
    return -1;
  }
  ptrdiff_t dir = 0;
  ptrdiff_t expansion_count = 0;
  struct Str rest = path;
  for (;;) {
    while (rest.len > 0 && rest.data[0] == '/') {
      rest = str_tail(rest, 1);
    }
    if (rest.len == 0) {
      // Only the root, or a path ending in `/`, `.` or `..`, ends here, and it names `dir` itself.
      lookup_out->node = dir;
      return 0;
    }
    const struct StrCut cut = str_cut(rest, '/');
    struct Str tail = cut.tail;
    while (tail.len > 0 && tail.data[0] == '/') {
      tail = str_tail(tail, 1);
    }
    const bool is_last = tail.len == 0;
    // A last component followed by `/` must name a directory, as `file/` fails with `ENOTDIR`, and
    // a symlink there is followed even when `is_follow` is not set, as `lstat("link/")` follows.
    const bool is_dir_required = is_last && cut.is_found;
    rest = tail;
    if (str_eq(cut.head, S("."))) {
      continue;
    }
    if (str_eq(cut.head, S(".."))) {
      dir = os->nodes[dir].parent;
      continue;
    }

    const struct OsMemoryEntry* entry = entry_find(os, dir, cut.head);
    if (entry == NULL) {
      if (is_last) {
        lookup_out->dir = dir;
        lookup_out->name = cut.head;
      }
      errno = ENOENT;
      return -1;
    }
    const struct OsMemoryNode* node = &os->nodes[entry->node];
    if (node->kind == OS_MEMORY_NODE_LINK && (!is_last || is_follow || is_dir_required)) {
      if (++expansion_count > OS_MEMORY_LINK_EXPANSION_COUNT_MAX) {
        errno = ELOOP;
        return -1;
      }
      // The target replaces the link's component, and the rest of the path continues from it. A
      // required directory keeps its `/`, so the target must be a directory too.
      rest = is_last && !is_dir_required
                 ? node->data
                 : str_concat(str_concat(node->data, S("/"), arena), rest, arena);
      if (node->data.len > 0 && node->data.data[0] == '/') {
        dir = 0;
      }
      continue;
    }
    if (is_last && (!is_dir_required || node->kind == OS_MEMORY_NODE_DIR)) {
      *lookup_out = (struct OsMemoryLookup){entry->node, dir, cut.head};
      return 0;
    }
    if (node->kind != OS_MEMORY_NODE_DIR) {
      errno = ENOTDIR;
      return -1;
    }
    dir = entry->node;
  }
}

static void node_add(struct Os* os,
                     ptrdiff_t dir,
                     struct Str name,
                     enum OsMemoryNodeKind kind,
                     struct Str data) {
  const ptrdiff_t node = node_new(os, kind, str_clone(data, &os->arena), dir);
  entry_add(os, dir, name, node);
}

static ptrdiff_t node_new(struct Os* os,
                          enum OsMemoryNodeKind kind,
                          struct Str data,
                          ptrdiff_t parent) {
  if (os->node_count == os->node_capacity) {
    os->nodes = arena_grow(&os->arena, os->nodes, &os->node_capacity, sizeof(*os->nodes),
                           _Alignof(struct OsMemoryNode));
  }
  const ptrdiff_t node = os->node_count++;
  os->nodes[node] =
      (struct OsMemoryNode){.kind = kind, .data = data, .parent = parent, .mtime = ++os->clock};
  return node;
}

static struct OsMemoryEntry* entry_find(struct Os* os, ptrdiff_t dir, struct Str name) {
  for (ptrdiff_t i = 0; i < os->entry_count; i++) {
    if (os->entries[i].dir == dir && str_eq(os->entries[i].name, name)) {
      return &os->entries[i];
    }
  }
  return NULL;
}

static void entry_add(struct Os* os, ptrdiff_t dir, struct Str name, ptrdiff_t node) {
  if (os->entry_count == os->entry_capacity) {
    os->entries = arena_grow(&os->arena, os->entries, &os->entry_capacity, sizeof(*os->entries),
                             _Alignof(struct OsMemoryEntry));
  }
  os->entries[os->entry_count++] = (struct OsMemoryEntry){dir, str_clone(name, &os->arena), node};
}

static const struct OsMemoryEntry* entry_of_lookup(struct Os* os,
                                                   const struct OsMemoryLookup* lookup) {
  if (lookup->dir < 0 || lookup->name.len == 0) {
    return NULL;
  }
  const struct OsMemoryEntry* entry = entry_find(os, lookup->dir, lookup->name);
  return entry != NULL && entry->node == lookup->node ? entry : NULL;
}

static void entry_remove(struct Os* os, const struct OsMemoryEntry* entry) {
  const ptrdiff_t index = entry - os->entries;
  memmove(&os->entries[index], &os->entries[index + 1],
          (size_t)(os->entry_count - index - 1) * sizeof(*entry));
  os->entry_count--;
}

static int fault_find(const struct Os* os, enum OsMemoryOp op, struct Str path) {
  for (ptrdiff_t i = 0; i < os->fault_count; i++) {
    const struct OsMemoryFault* fault = &os->faults[i];
    if (fault->op == op && str_startswith(path, fault->path) &&
        (path.len == fault->path.len || path.data[fault->path.len] == '/')) {
      return fault->error_number;
    }
  }
  return 0;
}

static int open_regular(struct Os* os,
                        struct Str file_path,
                        ptrdiff_t* node_out,
                        const struct OsMemoryReport* report) {
  const int fault = fault_find(os, OS_MEMORY_OP_READ, file_path);
  if (fault != 0) {
    return report_errno(report, file_path, fault);
  }
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup;
  if (tree_lookup(os, file_path, true, &scratch, &lookup) != 0) {
    return report_errno(report, file_path, errno);
  }
  if (os->nodes[lookup.node].kind != OS_MEMORY_NODE_FILE) {
    return log_error(report->log, "%.*s: not a regular file ('%.*s')", STR_FMT(report->operation),
                     STR_FMT(file_path));
  }
  *node_out = lookup.node;
  return 0;
}

static int os_write_locked(struct Os* os,
                           struct Str file_path,
                           struct OsSource source,
                           const struct OsMemoryReport* report) {
  // As in the POSIX implementation, a file source is opened first, and a path with no parent, or
  // with the root as its parent, creates nothing.
  ptrdiff_t source_node = -1;
  if (source.kind == OS_SOURCE_FILE &&
      open_regular(os, source.file_path, &source_node, report) != 0) {
    return -1;
  }
  if (mkdirs_report(os, path_parent(file_path), report) != 0) {
    return -1;
  }
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup = {.node = -1, .dir = -1};
  bool is_found = false;
  if (os_write_locked_open(os, file_path, &scratch, &lookup, &is_found, report) != 0) {
    return -1;
  }
  struct Str data = {0};
  ptrdiff_t zero_len = 0;
  const int rc = os_write_locked_data(os, file_path, source, source_node, &data, &zero_len, report);
  // As with `O_TRUNC`, a write that fails after the open leaves the file empty.
  os_write_locked_store(os, &lookup, is_found, data, zero_len);
  return rc;
}

static int os_write_locked_open(struct Os* os,
                                struct Str file_path,
                                struct Arena* arena,
                                struct OsMemoryLookup* lookup_out,
                                bool* is_found_out,
                                const struct OsMemoryReport* report) {
  const int fault = fault_find(os, OS_MEMORY_OP_WRITE, file_path);
  if (fault != 0) {
    return report_errno(report, file_path, fault);
  }
  struct OsMemoryLookup lookup;
  const bool is_found = tree_lookup(os, file_path, true, arena, &lookup) == 0;
  if (!is_found && (errno != ENOENT || lookup.dir < 0)) {
    return report_errno(report, file_path, errno);
  }
  if (is_found && os->nodes[lookup.node].kind == OS_MEMORY_NODE_DIR) {
    return report_errno(report, file_path, EISDIR);
  }
  *lookup_out = lookup;
  *is_found_out = is_found;
  return 0;
}

static int os_write_locked_data(struct Os* os,
                                struct Str file_path,
                                struct OsSource source,
                                ptrdiff_t source_node,
                                struct Str* data_out,
                                ptrdiff_t* zero_len_out,
                                const struct OsMemoryReport* report) {
  // An injected data failure writes nothing, as a write refused at its first byte does.
  const int fault = fault_find(os, OS_MEMORY_OP_WRITE_DATA, file_path);
  *data_out = (struct Str){0};
  *zero_len_out = 0;
  switch (source.kind) {
    case OS_SOURCE_BYTES:
      if (fault != 0) {
        return report_errno(report, file_path, fault);
      }
      *data_out = str_clone(source.bytes, &os->arena);
      return 0;
    case OS_SOURCE_FILE:
      if (fault != 0) {
        return report_errno(report, file_path, fault);
      }
      *data_out = os->nodes[source_node].data;
      *zero_len_out = os->nodes[source_node].zero_len;
      return 0;
  }
  return -1;
}

static void os_write_locked_store(struct Os* os,
                                  const struct OsMemoryLookup* lookup,
                                  bool is_found,
                                  struct Str data,
                                  ptrdiff_t zero_len) {
  if (is_found) {
    // The write goes through the existing node, so every hard link to it sees the new contents.
    struct OsMemoryNode* file = &os->nodes[lookup->node];
    file->data = data;
    file->zero_len = zero_len;
    file->mtime = ++os->clock;
    return;
  }
  // A missing file is created where the lookup ended, which for a dangling symlink is its target,
  // as `open` with `O_CREAT` does.
  const ptrdiff_t node = node_new(os, OS_MEMORY_NODE_FILE, data, lookup->dir);
  os->nodes[node].zero_len = zero_len;
  entry_add(os, lookup->dir, lookup->name, node);
}

static int mkdirs_report(struct Os* os, struct Str dir_path, const struct OsMemoryReport* report) {
  // As in the POSIX implementation, each `/` past the first byte ends one ancestor to create.
  for (ptrdiff_t i = 1; i < dir_path.len; i++) {
    if (dir_path.data[i] == '/' && mkdirs_report_one(os, str_head(dir_path, i), report) != 0) {
      return -1;
    }
  }
  return dir_path.len == 0 ? 0 : mkdirs_report_one(os, dir_path, report);
}

static int mkdirs_report_one(struct Os* os,
                             struct Str dir_path,
                             const struct OsMemoryReport* report) {
  struct Arena scratch = os->scratch;
  struct OsMemoryLookup lookup;
  if (tree_lookup(os, dir_path, false, &scratch, &lookup) != 0) {
    if (errno != ENOENT || lookup.dir < 0) {
      return report_step_errno(report, S("create directory"), dir_path, errno);
    }
    node_add(os, lookup.dir, lookup.name, OS_MEMORY_NODE_DIR, (struct Str){0});
    return 0;
  }
  // Something exists, which `mkdir` reports as `EEXIST`. The POSIX implementation then follows it
  // with `stat`, so a dangling symlink fails as an inspection, and anything else that is not a
  // directory fails as `exists and is not a directory`.
  if (os->nodes[lookup.node].kind == OS_MEMORY_NODE_LINK &&
      tree_lookup(os, dir_path, true, &scratch, &lookup) != 0) {
    return report_step_errno(report, S("inspect directory"), dir_path, errno);
  }
  if (os->nodes[lookup.node].kind != OS_MEMORY_NODE_DIR) {
    return log_error(report->log, "%.*s: exists and is not a directory ('%.*s')",
                     STR_FMT(report->operation), STR_FMT(dir_path));
  }
  return 0;
}

static enum ListClassify os_list_classify(struct Os* os,
                                          const struct OsMemoryEntry* entry,
                                          struct Str entry_path,
                                          struct Arena scratch,
                                          enum OsEntryKind* kind_out) {
  const int fault = fault_find(os, OS_MEMORY_OP_STAT, entry_path);
  if (fault != 0) {
    errno = fault;
    return LIST_CLASSIFY_FAILED;
  }
  ptrdiff_t node = entry->node;
  const bool is_link = os->nodes[node].kind == OS_MEMORY_NODE_LINK;
  if (is_link) {
    // A dangling or looping link is skipped, as the POSIX listing skips it, and a link through a
    // regular file fails as `ENOTDIR`, as it does there.
    struct OsMemoryLookup target;
    if (tree_lookup(os, entry_path, true, &scratch, &target) != 0) {
      return errno == ENOENT || errno == ELOOP ? LIST_CLASSIFY_SKIPPED : LIST_CLASSIFY_FAILED;
    }
    node = target.node;
  }
  if (os->nodes[node].kind == OS_MEMORY_NODE_DIR) {
    *kind_out = is_link ? OS_ENTRY_DIR_LINK : OS_ENTRY_DIR;
  } else {
    *kind_out = OS_ENTRY_FILE;
  }
  return LIST_CLASSIFY_LISTED;
}

static int report_errno(const struct OsMemoryReport* report, struct Str path, int error_number) {
  char message[ERROR_SYSTEM_MESSAGE_SIZE];
  return log_error(report->log, "%.*s: %s ('%.*s')", STR_FMT(report->operation),
                   error_system_message(message, sizeof(message), error_number), STR_FMT(path));
}

static int report_step_errno(const struct OsMemoryReport* report,
                             struct Str step,
                             struct Str path,
                             int error_number) {
  char message[ERROR_SYSTEM_MESSAGE_SIZE];
  return log_error(report->log, "%.*s: cannot %.*s: %s ('%.*s')", STR_FMT(report->operation),
                   STR_FMT(step), error_system_message(message, sizeof(message), error_number),
                   STR_FMT(path));
}
