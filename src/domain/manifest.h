#ifndef FRAM_MANIFEST_H
#define FRAM_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

/** One recorded output path and the source that claims it. Defined in `manifest.c`. */
struct ManifestEntry;

/** Insertion-ordered set of intended build output paths, each mapped to its producing source. */
struct Manifest {
  /**
   * Entries owned by the manifest, in insertion order. Nothing outside this module can read them.
   * `struct ManifestEntry` is incomplete here and no accessor exposes it, so this is internal state
   * rather than a list a caller can walk.
   */
  struct ManifestEntry* entries;

  /** Number of recorded output paths. */
  size_t count;

  /** Allocated slots in `entries`. */
  size_t capacity;

  /**
   * Open-addressing index over `entries`, keyed by the ASCII-folded output path. Each slot holds an
   * entry index plus one, or zero when empty. Managed internally, not for caller use.
   */
  size_t* buckets;

  /** Power-of-two number of slots in `buckets`. Managed internally, not for caller use. */
  size_t bucket_count;
};

/** Outcome of adding an output path to a manifest. */
enum ManifestAdd {
  /** The output path was new and is now recorded. */
  MANIFEST_ADD_INSERTED,

  /**
   * The output path, or one equal to it under ASCII case folding, was already claimed. The manifest
   * is unchanged.
   */
  MANIFEST_ADD_DUPLICATE,

  /** An allocation failed. No output path recorded. Internal capacity may already have grown. */
  MANIFEST_ADD_ERROR,
};

/**
 * @brief Initializes an empty manifest with no heap allocation.
 *
 * @param manifest Manifest handle to prepare. Must not be `NULL`.
 */
void manifest_init(struct Manifest* manifest) __attribute__((nonnull(1)));

/**
 * @brief Releases every recorded entry and resets the manifest for reuse.
 *
 * Leaves the manifest initialized, so it may be reused without `manifest_init`.
 *
 * @param manifest Manifest to release. Must not be `NULL`.
 */
void manifest_free(struct Manifest* manifest) __attribute__((nonnull(1)));

/**
 * @brief Records `output_path` and the source that produces it, rejecting a path already claimed
 *        under ASCII case folding.
 *
 * The manifest copies both strings, so the caller retains ownership of its arguments.
 * `manifest_add` reports a path already present as a duplicate and leaves the manifest unchanged.
 *
 * The manifest appends entries in insertion order, but that order is not reachable from outside
 * this module. `struct ManifestEntry` is incomplete and no accessor exposes it. Treat it as an
 * implementation property rather than part of this contract until one exists.
 *
 * Duplicate means equal after folding ASCII `A-Z` to `a-z`, so `Trips/Japan.JPG` and
 * `trips/japan.jpg` collide on every platform, as they would on a case-insensitive filesystem.
 * Every other byte compares exactly: the manifest does not attempt locale-dependent folding or
 * Unicode normalization. A `MANIFEST_ADD_INSERTED` is therefore not proof that the path can be
 * created: two outputs where one is a `/`-delimited prefix of the other, like a file `a/b` beside
 * `a/b/c`, are each recorded here yet still collide on disk. `manifest_find_prefix_collision`
 * catches that pair once every path is recorded.
 *
 * @param manifest                  Manifest to append to. Must not be `NULL`.
 * @param output_path               Filesystem output path to record. Must not be `NULL`.
 * @param source_label              Diagnostic label for the source producing `output_path`. Must
 *                                  not be `NULL`.
 * @param source_label_existing_out Receives the label of the earlier claim when the result is
 *                                  `MANIFEST_ADD_DUPLICATE`. Ignored when `NULL`. The label is
 *                                  borrowed, not transferred. It stays valid across further
 *                                  `manifest_add` calls, because growth reallocates the entry array
 *                                  but never the strings it points at. It dangles after
 *                                  `manifest_free`.
 * @return `MANIFEST_ADD_INSERTED` when the path was recorded, `MANIFEST_ADD_DUPLICATE` when it was
 *         already claimed, or `MANIFEST_ADD_ERROR` on allocation failure.
 */
enum ManifestAdd manifest_add(struct Manifest* manifest,
                              const char* output_path,
                              const char* source_label,
                              const char** source_label_existing_out)
    __attribute__((nonnull(1, 2, 3)));

/**
 * A pair of recorded output paths where one is a `/`-delimited prefix of the other under ASCII case
 * folding, so both cannot exist on disk: the shorter names a file. The longer needs that same name
 * as a directory.
 */
struct ManifestPrefixCollision {
  /** The shorter path, recorded as a file output. */
  const char* ancestor_path;

  /** Diagnostic label of the source that produced `ancestor_path`. */
  const char* ancestor_label;

  /** The longer path, which needs `ancestor_path` to be a directory. */
  const char* descendant_path;

  /** Diagnostic label of the source that produced `descendant_path`. */
  const char* descendant_label;
};

/**
 * @brief Finds a recorded output path that is a `/`-delimited prefix of another.
 *
 * `manifest_add` rejects only paths equal under ASCII case folding. This catches the other way two
 * outputs collide on disk: a file path that is also a directory prefix of a second output, like
 * `a/b` beside `a/b/c`. The prefix match folds ASCII case the same way, so `Assets/Icon` collides
 * with `assets/icon/image.jpg`. Run it once after every output path is recorded. It examines each
 * path's ancestor prefixes, so it reports such a pair whichever order the two were added in.
 *
 * The reported pointers are borrowed from the manifest and stay valid until `manifest_free`.
 *
 * @param manifest      Manifest to scan. Must not be `NULL`.
 * @param collision_out Receives the colliding pair on a hit. Untouched on a miss. Must not be
 *                      `NULL`.
 * @return `true` and fills `collision_out` when a prefix collision exists, or `false` when every
 *         recorded path can coexist on disk.
 */
bool manifest_find_prefix_collision(const struct Manifest* manifest,
                                    struct ManifestPrefixCollision* collision_out)
    __attribute__((nonnull(1, 2)));

#endif
