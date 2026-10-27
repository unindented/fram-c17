#include "domain/manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/ascii.h"
#include "shared/arena.h"
#include "shared/str.h"

/** Canonical 64-bit FNV-1a params (offset basis `0xcbf29ce484222325`, prime `0x100000001b3`). */
static const uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
static const uint64_t FNV_PRIME = 1099511628211ULL;

/**
 * One recorded output path and the source that claims it. Each entry is a node of the manifest's
 * hash trie and a link in its insertion-order list.
 */
struct ManifestEntry {
  /**
   * Subtries, selected by successive two-bit digits of the folded path's hash, high bits first. A
   * trie never rehashes or moves a node, so an entry and its strings stay where they were
   * allocated.
   */
  struct ManifestEntry* child[4];

  /** Next entry in insertion order, or `NULL` for the last. */
  struct ManifestEntry* next;

  /** Filesystem output path owned by the manifest's arena, in its original case. */
  struct Str output_path;

  /** Diagnostic source label owned by the manifest's arena. */
  struct Str source_label;
};

/**
 * @brief Descends the trie to the entry for `output_path` under ASCII case folding, or to the empty
 *        slot where that entry belongs.
 *
 * Each trie level consumes the top two bits of the hash. Paths that fold to the same bytes hash
 * alike, so they walk the same slots and the comparison finds the earlier claim. The descent ends
 * at the matching entry or at an empty slot, and the trie is at most as deep as it has entries, so
 * it always ends. The empty slot is reported as its parent and digit rather than as a pointer to
 * the slot, because the manifest is `const` here and only `manifest_add` writes the slot.
 *
 * @param manifest    Manifest to descend. Must not be `NULL`.
 * @param output_path Path to match.
 * @param parent_out  Receives the entry whose child the descent stopped at, or `NULL` when it
 *                    stopped at the root. Must not be `NULL`.
 * @param digit_out   Receives the index of that child in `parent_out`'s `child`. Must not be
 *                    `NULL`.
 * @return The matching entry, or `NULL` when the descent stopped at an empty slot.
 */
static struct ManifestEntry* descend_trie(const struct Manifest* manifest,
                                          struct Str output_path,
                                          struct ManifestEntry** parent_out,
                                          ptrdiff_t* digit_out) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Looks up the entry whose output path equals `output_path` under ASCII case folding.
 *
 * `manifest_find_prefix_collision` looks up an ancestor prefix as a view into the longer path, so
 * no prefix is copied.
 *
 * @param manifest    Manifest to query. Must not be `NULL`.
 * @param output_path Path to find.
 * @return The matching entry, or `NULL` when none matches.
 */
static const struct ManifestEntry* lookup_entry(const struct Manifest* manifest,
                                                struct Str output_path) __attribute__((nonnull(1)));

/**
 * @brief Reports whether two paths are equal under ASCII case folding.
 *
 * @param a First path.
 * @param b Second path.
 * @return `true` when both have the same length and each byte folds to the same value as its
 *         counterpart, `false` otherwise.
 */
static bool is_path_equal_folded(struct Str a, struct Str b);

/**
 * @brief Hashes a path with the 64-bit FNV-1a function under ASCII case folding.
 *
 * @param path Path to hash.
 * @return The 64-bit hash of the path with ASCII `A-Z` folded to `a-z`.
 */
static uint64_t hash_path_folded(struct Str path);

int manifest_add(struct Manifest* manifest,
                 struct Str output_path,
                 struct Str source_label,
                 struct Arena* arena,
                 struct Str* source_label_existing_out) {
  struct ManifestEntry* parent = NULL;
  ptrdiff_t digit = 0;
  const struct ManifestEntry* existing = descend_trie(manifest, output_path, &parent, &digit);
  if (existing != NULL) {
    if (source_label_existing_out != NULL) {
      *source_label_existing_out = existing->source_label;
    }
    return -1;
  }

  struct ManifestEntry* entry = arena_new(arena, 1, struct ManifestEntry);
  entry->output_path = str_clone(output_path, arena);
  entry->source_label = str_clone(source_label, arena);
  if (parent == NULL) {
    manifest->root = entry;
  } else {
    parent->child[digit] = entry;
  }
  if (manifest->tail == NULL) {
    manifest->head = entry;
  } else {
    manifest->tail->next = entry;
  }
  manifest->tail = entry;
  manifest->count++;
  return 0;
}

bool manifest_find_prefix_collision(const struct Manifest* manifest,
                                    struct ManifestPrefixCollision* collision) {
  // The previous descendant is a recorded path, so the lookup finds its entry, and the scan resumes
  // at the entry after it.
  const struct ManifestEntry* descendant = manifest->head;
  if (collision->descendant_path.data != NULL) {
    descendant = lookup_entry(manifest, collision->descendant_path)->next;
  }
  for (; descendant != NULL; descendant = descendant->next) {
    // Walk this path's ancestor prefixes at each `/`. If a prefix is itself a recorded output, that
    // output is a file this path needs to be a directory instead. Looking each prefix up in the
    // same index keeps the scan O(path length) per entry and independent of insertion order: for
    // any colliding pair the shorter path is an ancestor of the longer, so the longer entry finds
    // it here whichever was added first.
    const struct Str path = descendant->output_path;
    for (struct StrCut cut = str_cut(path, '/'); cut.is_found; cut = str_cut(cut.tail, '/')) {
      const struct Str prefix = str_head(path, cut.tail.data - 1 - path.data);
      const struct ManifestEntry* ancestor = lookup_entry(manifest, prefix);
      if (ancestor != NULL) {
        collision->ancestor_path = ancestor->output_path;
        collision->ancestor_label = ancestor->source_label;
        collision->descendant_path = descendant->output_path;
        collision->descendant_label = descendant->source_label;
        return true;
      }
    }
  }
  return false;
}

bool manifest_next_output_path(const struct Manifest* manifest, struct Str* output_path) {
  // The previous path is a recorded one, so the lookup finds its entry, as in
  // `manifest_find_prefix_collision`.
  const struct ManifestEntry* entry =
      output_path->data == NULL ? manifest->head : lookup_entry(manifest, *output_path)->next;
  if (entry == NULL) {
    return false;
  }
  *output_path = entry->output_path;
  return true;
}

static struct ManifestEntry* descend_trie(const struct Manifest* manifest,
                                          struct Str output_path,
                                          struct ManifestEntry** parent_out,
                                          ptrdiff_t* digit_out) {
  struct ManifestEntry* parent = NULL;
  ptrdiff_t digit = 0;
  struct ManifestEntry* entry = manifest->root;
  for (uint64_t hash = hash_path_folded(output_path); entry != NULL; hash <<= 2) {
    if (is_path_equal_folded(entry->output_path, output_path)) {
      break;
    }
    parent = entry;
    digit = (ptrdiff_t)(hash >> 62);
    entry = entry->child[digit];
  }
  *parent_out = parent;
  *digit_out = digit;
  return entry;
}

static const struct ManifestEntry* lookup_entry(const struct Manifest* manifest,
                                                struct Str output_path) {
  struct ManifestEntry* parent = NULL;
  ptrdiff_t digit = 0;
  return descend_trie(manifest, output_path, &parent, &digit);
}

static bool is_path_equal_folded(struct Str a, struct Str b) {
  if (a.len != b.len) {
    return false;
  }
  for (ptrdiff_t i = 0; i < a.len; i++) {
    if (ascii_to_lower((unsigned char)a.data[i]) != ascii_to_lower((unsigned char)b.data[i])) {
      return false;
    }
  }
  return true;
}

static uint64_t hash_path_folded(struct Str path) {
  uint64_t hash = FNV_OFFSET_BASIS;
  // This reads through `unsigned char` because plain `char` may be signed, in which case a byte
  // above `0x7f` would sign-extend and make the hash depend on the platform's `char` signedness.
  // Folding here, with the same `ascii_to_lower` that `is_path_equal_folded` compares under, is
  // what lets two paths that differ only in ASCII case walk the same trie slots. The multiply
  // relies on wrapping modulo 2^64, which is defined for unsigned types only. The same expression
  // on a signed type would be undefined on overflow, so `uint64_t` here is a correctness
  // requirement, not a width preference.
  for (ptrdiff_t i = 0; i < path.len; i++) {
    hash ^= (uint64_t)ascii_to_lower((unsigned char)path.data[i]);
    hash *= FNV_PRIME;
  }
  return hash;
}
