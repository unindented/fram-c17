#include "build/output_pruner.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/error.h"
#include "core/path.h"
#include "domain/gallery_config.h"
#include "domain/manifest.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"

/** Number of input roots the prune never enters: `input_dir`, `templates_dir`, `static_dir`. */
enum { PRUNE_ROOT_COUNT = 3 };

/** What one prune keeps and where it reports, gathered before the walk. */
struct Prune {
  /**
   * Identities of every output and of every directory between `output_dir` and one, sorted for a
   * bisection. A rebuild walks every entry of the output tree, and a linear scan over the outputs
   * for each of them would grow with the square of its size.
   */
  struct OsIdentity* kept_items;

  /** Number of identities in `kept_items`. */
  ptrdiff_t kept_count;

  /** Identities of the input roots that exist. */
  struct OsIdentity root_items[PRUNE_ROOT_COUNT];

  /** Number of identities in `root_items`. */
  ptrdiff_t root_count;

  /** Host the output tree is on. */
  struct Os* os;

  /** Number of entries removed so far. */
  ptrdiff_t removed_count;

  /** Whether any listing or removal failed. */
  bool is_failed;

  /** Log that receives every failure. */
  struct Log* log;
};

/**
 * @brief Records the identity of every output and of every directory between `output_dir` and one.
 *
 * Consecutive outputs usually share a directory, so a path whose directory is the previous path's
 * adds only its own identity. A path with no identity, which a write that did not happen leaves, is
 * skipped.
 *
 * @param prune      Prune that receives the sorted identities. Must not be `NULL`.
 * @param manifest   Every output path, each joined onto `output_dir`. Must not be `NULL`.
 * @param output_dir Directory the output paths are below.
 * @param scratch    Arena that owns the identities. Must not be `NULL`.
 */
static void claim_kept_outputs(struct Prune* prune,
                               const struct Manifest* manifest,
                               struct Str output_dir,
                               struct Arena* scratch) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Records one path's identity, following symlinks, ignoring a path that has none.
 *
 * @param prune Prune whose `kept_items` has a free slot. Must not be `NULL`.
 * @param path  Path whose identity is kept.
 */
static void claim_kept_outputs_identity(struct Prune* prune, struct Str path)
    __attribute__((nonnull(1)));

/**
 * @brief Records the identity of each input root that exists.
 *
 * @param prune          Prune that receives the identities. Must not be `NULL`.
 * @param gallery_config Configuration supplying the roots. Must not be `NULL`.
 */
static void claim_input_roots(struct Prune* prune, const struct GalleryConfig* gallery_config)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Removes the stale entries below one directory.
 *
 * @param prune    Prune in progress. Must not be `NULL`.
 * @param dir_path Directory to prune.
 * @param scratch  Arena for the listing and the entry paths.
 * @return `true` when every entry below `dir_path` was removed, or `false` when one was kept or
 *         could not be listed or removed.
 */
static bool prune_dir(struct Prune* prune, struct Str dir_path, struct Arena scratch)
    __attribute__((nonnull(1)));

/**
 * @brief Removes one listed entry unless the build wrote it or passes through it.
 *
 * @param prune      Prune in progress. Must not be `NULL`.
 * @param entry      Listed entry. Must not be `NULL`.
 * @param entry_path Path of the entry.
 * @param scratch    Arena for the listing below a directory entry.
 * @return `true` when the entry is gone, or `false` when it was kept or could not be removed.
 */
static bool prune_dir_entry(struct Prune* prune,
                            const struct OsEntry* entry,
                            struct Str entry_path,
                            struct Arena scratch) __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether following `path` reaches an output or a directory one is below.
 *
 * @param prune Prune whose identities are searched. Must not be `NULL`.
 * @param path  Path to inspect.
 * @return `true` when the path is kept, `false` when it is stale or has no identity.
 */
static bool is_kept(const struct Prune* prune, struct Str path) __attribute__((nonnull(1)));

/**
 * @brief Reports whether `identity` names an input root.
 *
 * @param prune    Prune whose roots are searched. Must not be `NULL`.
 * @param identity Identity to look for. Must not be `NULL`.
 * @return `true` when `identity` is an input root, otherwise `false`.
 */
static bool is_input_root(const struct Prune* prune, const struct OsIdentity* identity)
    __attribute__((nonnull(1, 2)));

int output_pruner_prune(const struct Manifest* manifest,
                        const struct GalleryConfig* gallery_config,
                        struct Os* os,
                        struct Arena scratch,
                        ptrdiff_t* removed_count_out,
                        struct Log* log) {
  struct Prune prune = {.os = os, .log = log};
  claim_kept_outputs(&prune, manifest, gallery_config->output_dir, &scratch);
  claim_input_roots(&prune, gallery_config);
  (void)prune_dir(&prune, gallery_config->output_dir, scratch);
  *removed_count_out = prune.removed_count;
  return prune.is_failed ? -1 : 0;
}

static void claim_kept_outputs(struct Prune* prune,
                               const struct Manifest* manifest,
                               struct Str output_dir,
                               struct Arena* scratch) {
  // One slot for each output and for each directory component below `output_dir` in its path,
  // counted before any identity is taken, so the array is allocated once.
  ptrdiff_t slot_count = 0;
  struct Str output_path = {0};
  while (manifest_next_output_path(manifest, &output_path)) {
    struct Str relative_path = {0};
    (void)path_is_below(output_path, output_dir, &relative_path);
    slot_count++;
    for (ptrdiff_t i = 0; i < relative_path.len; i++) {
      slot_count += relative_path.data[i] == '/';
    }
  }
  prune->kept_items = arena_new(scratch, slot_count, struct OsIdentity);

  struct Str previous_dir = {0};
  output_path = (struct Str){0};
  while (manifest_next_output_path(manifest, &output_path)) {
    claim_kept_outputs_identity(prune, output_path);
    const struct Str dir_path = path_parent(output_path);
    if (previous_dir.data != NULL && str_eq(dir_path, previous_dir)) {
      continue;
    }
    previous_dir = dir_path;
    struct Str relative_path = {0};
    (void)path_is_below(output_path, output_dir, &relative_path);
    const ptrdiff_t relative_start = output_path.len - relative_path.len;
    for (ptrdiff_t i = 0; i < relative_path.len; i++) {
      if (relative_path.data[i] == '/') {
        claim_kept_outputs_identity(prune, str_head(output_path, relative_start + i));
      }
    }
  }
  if (prune->kept_count > 0) {
    qsort(prune->kept_items, (size_t)prune->kept_count, sizeof(*prune->kept_items),
          os_identity_compare);
  }
}

static void claim_kept_outputs_identity(struct Prune* prune, struct Str path) {
  struct OsStat st;
  if (os_stat(prune->os, path, &st) == 0) {
    prune->kept_items[prune->kept_count++] = st.identity;
  }
}

static void claim_input_roots(struct Prune* prune, const struct GalleryConfig* gallery_config) {
  const struct Str root_dirs[PRUNE_ROOT_COUNT] = {
      gallery_config->input_dir, gallery_config->templates_dir, gallery_config->static_dir};
  for (ptrdiff_t i = 0; i < PRUNE_ROOT_COUNT; i++) {
    struct OsStat st;
    if (os_stat(prune->os, root_dirs[i], &st) == 0) {
      prune->root_items[prune->root_count++] = st.identity;
    }
  }
}

static bool prune_dir(struct Prune* prune, struct Str dir_path, struct Arena scratch) {
  struct OsEntry* entries = NULL;
  ptrdiff_t entry_count = 0;
  if (os_list(prune->os, dir_path, &scratch, &entries, &entry_count,
              S("failed to list stale outputs"), prune->log) != 0) {
    prune->is_failed = true;
    return false;
  }
  bool is_emptied = true;
  for (ptrdiff_t i = 0; i < entry_count; i++) {
    struct Arena entry_scratch = scratch;
    const struct Str entry_path = path_join(dir_path, entries[i].name, &entry_scratch);
    if (!prune_dir_entry(prune, &entries[i], entry_path, entry_scratch)) {
      is_emptied = false;
    }
  }
  return is_emptied;
}

static bool prune_dir_entry(struct Prune* prune,
                            const struct OsEntry* entry,
                            struct Str entry_path,
                            struct Arena scratch) {
  switch (entry->kind) {
    case OS_ENTRY_FAILED: {
      char message[ERROR_SYSTEM_MESSAGE_SIZE];
      (void)log_error(prune->log, "failed to list stale outputs: cannot inspect entry: %s ('%.*s')",
                      error_system_message(message, sizeof(message), entry->error_number),
                      STR_FMT(entry_path));
      prune->is_failed = true;
      return false;
    }
    case OS_ENTRY_FILE:
    case OS_ENTRY_DIR_LINK:
      // Either may be a symlink. `os_remove` removes the link itself, so a link that leads out of
      // the output tree removes nothing out there.
      if (is_kept(prune, entry_path)) {
        return false;
      }
      if (os_remove(prune->os, entry_path, S("failed to remove stale output"), prune->log) != 0) {
        prune->is_failed = true;
        return false;
      }
      prune->removed_count++;
      return true;
    case OS_ENTRY_DIR: {
      struct OsStat st;
      if (os_stat(prune->os, entry_path, &st) == 0 && is_input_root(prune, &st.identity)) {
        return false;
      }
      if (is_kept(prune, entry_path)) {
        (void)prune_dir(prune, entry_path, scratch);
        return false;
      }
      // A directory left non-empty by a failure below it would fail to go for the same cause, so it
      // is not tried, and that cause is reported once.
      if (!prune_dir(prune, entry_path, scratch)) {
        return false;
      }
      if (os_remove_dir(prune->os, entry_path, S("failed to remove stale output"), prune->log) !=
          0) {
        prune->is_failed = true;
        return false;
      }
      prune->removed_count++;
      return true;
    }
  }
  return false;
}

static bool is_kept(const struct Prune* prune, struct Str path) {
  struct OsStat st;
  return prune->kept_count > 0 && os_stat(prune->os, path, &st) == 0 &&
         bsearch(&st.identity, prune->kept_items, (size_t)prune->kept_count,
                 sizeof(*prune->kept_items), os_identity_compare) != NULL;
}

static bool is_input_root(const struct Prune* prune, const struct OsIdentity* identity) {
  for (ptrdiff_t i = 0; i < prune->root_count; i++) {
    if (os_identity_compare(&prune->root_items[i], identity) == 0) {
      return true;
    }
  }
  return false;
}
