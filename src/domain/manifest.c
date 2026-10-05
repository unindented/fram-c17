#include "domain/manifest.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/ascii.h"
#include "core/grow.h"
#include "core/text.h"

/** First slot count allocated when an empty entry array first grows. */
enum { MANIFEST_ENTRIES_CAPACITY_MIN = 16 };

/**
 * First bucket count allocated when the hash index first grows. It must be a power of two, because
 * `manifest_bucket_place` selects a bucket by masking rather than by remainder. It is chosen as
 * twice `MANIFEST_ENTRIES_CAPACITY_MIN` so the first full entry array fits under the 3/4 load
 * factor (24 of 32 slots) and a small gallery never rehashes.
 */
enum { MANIFEST_BUCKETS_CAPACITY_MIN = 32 };

// Doubling in `manifest_reserve_buckets` preserves the property, so the seed is the only bucket
// count that needs checking.
_Static_assert((MANIFEST_BUCKETS_CAPACITY_MIN & (MANIFEST_BUCKETS_CAPACITY_MIN - 1)) == 0,
               "manifest bucket selection masks, so the bucket count must be a power of two");

/** Canonical 64-bit FNV-1a params (offset basis `0xcbf29ce484222325`, prime `0x100000001b3`). */
static const uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
static const uint64_t FNV_PRIME = 1099511628211ULL;

/** One recorded output path and the source that claims it. */
struct ManifestEntry {
  /** Filesystem output path owned by the manifest, in its original case. */
  char* output_path;

  /** Diagnostic source label owned by the manifest. */
  char* source_label;
};

/**
 * @brief Looks up the entry recorded for an output path under ASCII case folding.
 *
 * @param manifest    Manifest to query. Must not be `NULL`.
 * @param output_path Output path to look up. Must not be `NULL`.
 * @return The entry whose path equals `output_path` after folding ASCII case, or `NULL` when none
 *         does. The pointer points into `manifest->entries`, which `manifest_reserve_entries` may
 *         `realloc`, so reading it after any growth is a use-after-free. Consume it before
 *         reserving. `manifest_add` is correct only because it does exactly that.
 */
static const struct ManifestEntry* manifest_lookup(const struct Manifest* manifest,
                                                   const char* output_path)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Looks up the entry whose output path equals the first `len` bytes of `output_path` under
 *        ASCII case folding.
 *
 * The length-aware core of `manifest_lookup`, also used by `manifest_find_prefix_collision` to look
 * up an ancestor prefix without copying it out of the longer path. Matches only a recorded path of
 * exactly `output_path_len` bytes, never a longer one that merely starts with them.
 *
 * @param manifest        Manifest to query. Must not be `NULL`.
 * @param output_path     Buffer holding the path bytes to match. Must not be `NULL`.
 * @param output_path_len Number of leading bytes of `output_path` that form the path to find.
 * @return The matching entry, or `NULL`. Same reuse-before-growth caveat as `manifest_lookup`.
 */
static const struct ManifestEntry* manifest_lookup_bytes(const struct Manifest* manifest,
                                                         const char* output_path,
                                                         size_t output_path_len)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether a terminated candidate equals a byte range under ASCII case folding.
 *
 * @param candidate Terminated recorded path to compare. Must not be `NULL`.
 * @param bytes     Buffer holding the path bytes to match. Must not be `NULL`.
 * @param bytes_len Number of leading bytes of `bytes` to compare.
 * @return `true` when `candidate` is exactly `bytes_len` bytes long and each byte folds to the same
 *         value as its counterpart in `bytes`, `false` otherwise.
 */
static bool manifest_equal_bytes(const char* candidate, const char* bytes, size_t bytes_len)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Ensures the entry array has room for one more entry.
 *
 * @param manifest Manifest whose entry array is grown as needed. Must not be `NULL`.
 * @return `0` on success, or `-1` on size overflow or allocation failure.
 */
static int manifest_reserve_entries(struct Manifest* manifest) __attribute__((nonnull(1)));

/**
 * @brief Grows and rehashes the hash index so it stays below its load factor.
 *
 * Reserves room for one more entry, leaving the table unchanged when it already has capacity.
 *
 * @param manifest Manifest whose bucket table is grown and rehashed as needed. Must not be `NULL`.
 * @return `0` on success, or `-1` on size overflow or allocation failure.
 */
static int manifest_reserve_buckets(struct Manifest* manifest) __attribute__((nonnull(1)));

/**
 * @brief Places an entry index into the first empty bucket on its probe chain.
 *
 * Requires the table to hold at least one empty bucket, which the load factor in
 * `manifest_reserve_buckets` guarantees. A table with none is a broken invariant. This stops the
 * process rather than leave the entry unindexed.
 *
 * @param buckets     Bucket table to insert into. Must not be `NULL`.
 * @param bucket_mask Power-of-two table length minus one, used to wrap the linear probe.
 * @param output_path Output path whose folded hash selects the starting bucket. Must not be `NULL`.
 * @param index       Zero-based entry index. Stored as `index + 1` so zero stays the empty marker.
 */
static void manifest_bucket_place(size_t* buckets,
                                  size_t bucket_mask,
                                  const char* output_path,
                                  size_t index) __attribute__((nonnull(1, 3)));

/**
 * @brief Hashes a terminated string with the 64-bit FNV-1a function under ASCII case folding.
 *
 * @param output_path Output path to hash. Must not be `NULL`.
 * @return The 64-bit hash of `output_path` with ASCII `A-Z` folded to `a-z`.
 */
static uint64_t manifest_hash(const char* output_path) __attribute__((nonnull(1)));

/**
 * @brief Hashes the first `bytes_len` bytes of `bytes` with the 64-bit FNV-1a function under ASCII
 *        case folding.
 *
 * @param bytes     Buffer to hash. Must not be `NULL`.
 * @param bytes_len Number of bytes to hash.
 * @return The 64-bit hash of the byte range with ASCII `A-Z` folded to `a-z`.
 */
static uint64_t manifest_hash_bytes(const char* bytes, size_t bytes_len)
    __attribute__((nonnull(1)));

void manifest_init(struct Manifest* manifest) {
  *manifest = (struct Manifest){0};
}

void manifest_free(struct Manifest* manifest) {
  for (size_t i = 0; i < manifest->count; i++) {
    free(manifest->entries[i].output_path);
    free(manifest->entries[i].source_label);
  }
  free(manifest->entries);
  free(manifest->buckets);
  manifest_init(manifest);
}

enum ManifestAdd manifest_add(struct Manifest* manifest,
                              const char* output_path,
                              const char* source_label,
                              const char** source_label_existing_out) {
  // Check for a duplicate before reserving so a growth-triggered allocation failure cannot mask an
  // already-present path as `MANIFEST_ADD_ERROR`. A duplicate leaves the manifest unchanged.
  const struct ManifestEntry* entry_existing = manifest_lookup(manifest, output_path);
  if (entry_existing != NULL) {
    if (source_label_existing_out != NULL) {
      *source_label_existing_out = entry_existing->source_label;
    }
    return MANIFEST_ADD_DUPLICATE;
  }

  if (manifest_reserve_entries(manifest) != 0 || manifest_reserve_buckets(manifest) != 0) {
    return MANIFEST_ADD_ERROR;
  }

  char* output_path_copy = text_strdup(output_path);
  char* source_label_copy = text_strdup(source_label);
  if (output_path_copy == NULL || source_label_copy == NULL) {
    free(output_path_copy);
    free(source_label_copy);
    return MANIFEST_ADD_ERROR;
  }

  const size_t index = manifest->count;
  manifest->entries[index].output_path = output_path_copy;
  manifest->entries[index].source_label = source_label_copy;
  manifest->count++;

  manifest_bucket_place(manifest->buckets, manifest->bucket_count - 1, output_path, index);
  return MANIFEST_ADD_INSERTED;
}

bool manifest_find_prefix_collision(const struct Manifest* manifest,
                                    struct ManifestPrefixCollision* collision_out) {
  for (size_t i = 0; i < manifest->count; i++) {
    const struct ManifestEntry* descendant = &manifest->entries[i];
    // Walk this path's ancestor prefixes at each `/`. If a prefix is itself a recorded output, that
    // output is a file this path needs to be a directory instead. Looking each prefix up in the
    // same index keeps the scan O(path length) per entry and independent of insertion order: for
    // any colliding pair the shorter path is an ancestor of the longer, so the longer entry finds
    // it here whichever was added first.
    for (const char* slash = strchr(descendant->output_path, '/'); slash != NULL;
         slash = strchr(slash + 1, '/')) {
      const size_t prefix_len = (size_t)(slash - descendant->output_path);
      const struct ManifestEntry* ancestor =
          manifest_lookup_bytes(manifest, descendant->output_path, prefix_len);
      if (ancestor != NULL) {
        collision_out->ancestor_path = ancestor->output_path;
        collision_out->ancestor_label = ancestor->source_label;
        collision_out->descendant_path = descendant->output_path;
        collision_out->descendant_label = descendant->source_label;
        return true;
      }
    }
  }
  return false;
}

static const struct ManifestEntry* manifest_lookup(const struct Manifest* manifest,
                                                   const char* output_path) {
  return manifest_lookup_bytes(manifest, output_path, strlen(output_path));
}

static const struct ManifestEntry* manifest_lookup_bytes(const struct Manifest* manifest,
                                                         const char* output_path,
                                                         size_t output_path_len) {
  if (manifest->bucket_count == 0) {
    return NULL;
  }
  const size_t bucket_mask = manifest->bucket_count - 1;
  size_t slot = (size_t)(manifest_hash_bytes(output_path, output_path_len) & bucket_mask);
  // The loop bounds itself by the table length rather than trusting an empty bucket to stop it.
  // After `bucket_count` probes the loop has inspected every slot, so it has nothing left to look
  // at. The load factor in `manifest_reserve_buckets` makes the bound unreachable. If that load
  // factor regresses, the bound makes this operation fail instead of loop indefinitely.
  for (size_t probe = 0; probe <= bucket_mask; probe++) {
    const size_t entry = manifest->buckets[slot];
    if (entry == 0) {
      return NULL;
    }
    // A candidate matches only when its whole path is these `output_path_len` bytes: equal over the
    // range and terminated right after, so a longer recorded path that merely starts with them is
    // not a hit. That exact-length test is what lets `manifest_find_prefix_collision` reuse this to
    // look up an ancestor prefix rather than any path sharing a leading run.
    const struct ManifestEntry* candidate = &manifest->entries[entry - 1];
    if (manifest_equal_bytes(candidate->output_path, output_path, output_path_len)) {
      return candidate;
    }
    slot = (slot + 1) & bucket_mask;
  }
  return NULL;
}

static bool manifest_equal_bytes(const char* candidate, const char* bytes, size_t bytes_len) {
  for (size_t i = 0; i < bytes_len; i++) {
    if (candidate[i] == '\0' ||
        ascii_to_lower((unsigned char)candidate[i]) != ascii_to_lower((unsigned char)bytes[i])) {
      return false;
    }
  }
  return candidate[bytes_len] == '\0';
}

static int manifest_reserve_entries(struct Manifest* manifest) {
  if (manifest->count < manifest->capacity) {
    return 0;
  }
  size_t capacity_next = 0;
  size_t capacity_bytes_next = 0;
  if (grow_capacity(manifest->capacity, MANIFEST_ENTRIES_CAPACITY_MIN, sizeof(*manifest->entries),
                    &capacity_next, &capacity_bytes_next) != 0) {
    return -1;
  }
  // Assigning into a fresh local rather than the field keeps a failed `realloc` recoverable.
  // `manifest->entries` still points at the valid old block, which `manifest_free` will release.
  struct ManifestEntry* entries = realloc(manifest->entries, capacity_bytes_next);
  if (entries == NULL) {
    return -1;
  }
  manifest->capacity = capacity_next;
  manifest->entries = entries;
  return 0;
}

static int manifest_reserve_buckets(struct Manifest* manifest) {
  // `manifest_reserve_entries` runs first and leaves `count` below an entry capacity that
  // `grow_capacity` bounded, so `count + 1` cannot wrap.
  const size_t count_next = manifest->count + 1;
  // Keep the table below a 3/4 load factor. Short probe chains are the performance reason. The
  // correctness reason is that a table never full leaves at least one empty bucket.
  // `manifest_bucket_place` needs one to index a new entry, and aborts without it, and a
  // `manifest_lookup_bytes` miss otherwise scans every bucket before its bound stops it. This is
  // written as `/ 4 * 3` rather than `* 3 / 4` so a large `bucket_count` cannot wrap the multiply.
  if (manifest->bucket_count != 0 && count_next <= (manifest->bucket_count / 4) * 3) {
    return 0;
  }
  // The mask probe requires `bucket_count` to stay a power of two. The seed is one, and doubling
  // keeps it one. `test_grow` pins that `grow_capacity` doubles.
  size_t bucket_count_next = 0;
  size_t buckets_bytes_next = 0;
  if (grow_capacity(manifest->bucket_count, MANIFEST_BUCKETS_CAPACITY_MIN,
                    sizeof(*manifest->buckets), &bucket_count_next, &buckets_bytes_next) != 0) {
    return -1;
  }
  size_t* buckets = calloc(1, buckets_bytes_next);
  if (buckets == NULL) {
    return -1;
  }

  const size_t bucket_mask = bucket_count_next - 1;
  for (size_t i = 0; i < manifest->count; i++) {
    manifest_bucket_place(buckets, bucket_mask, manifest->entries[i].output_path, i);
  }
  free(manifest->buckets);
  manifest->buckets = buckets;
  manifest->bucket_count = bucket_count_next;
  return 0;
}

static void manifest_bucket_place(size_t* buckets,
                                  size_t bucket_mask,
                                  const char* output_path,
                                  size_t index) {
  size_t slot = (size_t)(manifest_hash(output_path) & bucket_mask);
  // This is bounded like `manifest_lookup_bytes`'s probe, for the same reason. Reaching the bound
  // means the table holds no empty bucket, which `manifest_reserve_buckets` must have ruled out
  // before this runs. There is no recovery to return. The caller has already stored the entry and
  // bumped `count`, so a failure here would leave a recorded entry that no bucket points at. An
  // unindexed entry is an output path whose next claimant `manifest_add` never reports as a
  // duplicate. A broken invariant would therefore hide a duplicate output path, so the process
  // stops instead.
  for (size_t probe = 0; probe <= bucket_mask; probe++) {
    if (buckets[slot] == 0) {
      buckets[slot] = index + 1;
      return;
    }
    slot = (slot + 1) & bucket_mask;
  }
  abort();
}

static uint64_t manifest_hash(const char* output_path) {
  return manifest_hash_bytes(output_path, strlen(output_path));
}

static uint64_t manifest_hash_bytes(const char* bytes, size_t bytes_len) {
  uint64_t hash = FNV_OFFSET_BASIS;
  // This reads through `unsigned char` because plain `char` may be signed, in which case a byte
  // above `0x7f` would sign-extend and make the hash depend on the platform's `char` signedness.
  // Folding here, with the same `ascii_to_lower` that `manifest_equal_bytes` compares under, is
  // what lets two paths that differ only in ASCII case land on the same probe chain. The multiply
  // relies on wrapping modulo 2^64, which is defined for unsigned types only. The same expression
  // on a signed type would be undefined on overflow, so `uint64_t` here is a correctness
  // requirement, not a width preference.
  for (size_t i = 0; i < bytes_len; i++) {
    hash ^= (uint64_t)ascii_to_lower((unsigned char)bytes[i]);
    hash *= FNV_PRIME;
  }
  return hash;
}
