#include "build/manifest_builder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build/output_path.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/manifest.h"
#include "domain/media_item.h"
#include "runtime/fs.h"
#include "shared/arena.h"

/**
 * Size in bytes of the diagnostic label naming one configured template list entry, including the
 * `NUL` terminator. The `<config key>[<index>]` label tells two colliding template outputs apart. A
 * bare template name cannot: one name listed twice in `aggregate_templates` is a different mistake
 * from a template whose output collides with an album or asset, with a different fix, and it would
 * otherwise report as a collision between a value and itself. The name is left out of the label
 * because it is already the tail of the output path these messages trail.
 */
enum { TEMPLATE_SOURCE_LABEL_SIZE = 64 };

// The longest key plus a decimal `size_t` at its widest, so a label formatted in
// `manifest_builder_populate` cannot truncate and its `snprintf` result needs no check.
_Static_assert(TEMPLATE_SOURCE_LABEL_SIZE >
                   sizeof("aggregate_templates") - 1 + sizeof("[18446744073709551615]") - 1,
               "template source label buffer must hold the longest key and a size_t index");

/**
 * Identities of the files this build reads, gathered before any output path is registered so an
 * intended output naming one of them can be rejected before anything is written.
 *
 * Path strings cannot reliably identify the same file. An output path is relative to `output_dir`,
 * but a source path is relative to `input_dir`. For example, `output_dir = "."` can produce
 * `./photos/x.jpg` for the source `photos/x.jpg`. That is one file with two spellings that are not
 * byte-equal. A `(device, inode)` comparison also detects cases that text cannot. These include
 * symlinks, hard links, and case-insensitive path aliases.
 *
 * The input roots reject any output below `input_dir`, `templates_dir`, or `assets_dir` first, so
 * this set is what still protects an input outside all three trees: the configuration file, and the
 * target of a source, template, or asset that is a symlink or hard link to a file elsewhere.
 *
 * The identities are sorted once after they are all claimed, and each lookup is a binary search. A
 * rebuild looks up every output that already exists, which is every output of a gallery built
 * before: about 80,000 at 20,000 photos with three derivatives and an original each. A linear scan
 * over the 20,000 inputs made that 1.6 billion comparisons. Measured in a release build,
 * `manifest_builder_populate` on that no-op rebuild took 1.10 seconds with a linear scan and 0.24
 * seconds with the binary search. Most of the 0.24 seconds is one `fs_identify` call per output.
 */
struct InputIdentities {
  /**
   * Identities owned by this set, allocated once with a slot for every candidate input. In claim
   * order until `claim_build_inputs` sorts them by device and then inode for `has_input_identity`'s
   * binary search.
   */
  struct FsIdentity* items;

  /** Number of recorded identities. */
  size_t count;
};

/**
 * Number of input directory trees no output may enter: `input_dir`, `templates_dir`, and
 * `assets_dir`.
 */
enum { INPUT_ROOT_COUNT = 3 };

/** One input directory tree that no output may land inside. */
struct InputRoot {
  /** Config key naming the root in diagnostics. */
  const char* config_key;

  /** Identity of the root directory. Set only when `is_present` is `true`. */
  struct FsIdentity identity;

  /** Whether the root exists. A missing root holds no input to protect. */
  bool is_present;
};

/**
 * Input directory trees that no output may land at or below, gathered before any output path is
 * registered.
 *
 * Protecting the files that already exist is not enough. A new output below `input_dir` can be
 * discovered as a media source by the next build, and a new output below `assets_dir` is copied as
 * an asset by the next build. A new output below `templates_dir` can be read as a partial by the
 * render of the same build, because partials resolve lazily. None of them overwrites a file, so the
 * identity set cannot see any of them.
 *
 * Each root is compared by identity against the directories an output passes through, for the same
 * reasons the identity set is. `./photos`, `photos/`, a symlinked root, and a case-insensitive
 * alias spell one directory in different ways. A root that holds `output_dir` holds every output,
 * so `manifest_builder_check_output_dir` answers that once, against `output_dir` and each of its
 * physical ancestors. The build runs it before it walks any input tree, and
 * `manifest_builder_populate` runs it again so that no caller can skip it. Populate then compares
 * each directory between `output_dir` and the output file, which is where a symlink inside the
 * output tree can still lead into a root.
 */
struct InputRoots {
  /** The `input_dir`, `templates_dir`, and `assets_dir` roots. */
  struct InputRoot items[INPUT_ROOT_COUNT];

  /** Output directory every registered output path is joined onto. */
  const char* output_dir;
};

/**
 * @brief Records the identity of every file this build reads.
 *
 * Claims the configuration file, each discovered media source, each enumerated asset, and every
 * file below `templates_dir`.
 *
 * This claims the configuration file for the same reason as the rest. It is a file this build
 * reads, and `output_dir = "."` with an aggregate template named after it is enough to aim an
 * output path at it. If unclaimed, that output would overwrite the gallery's configuration. The
 * build would still report success.
 *
 * The whole template tree is claimed rather than the configured template names, because partials
 * are inputs too. `template.c` resolves them lazily during the render, from names that only appear
 * inside a template's bytes, so no phase that runs before the first write can enumerate the
 * partials a build will read. Claiming every file below `templates_dir` covers them without that
 * list, so an output path aimed inside `templates_dir/partials/` is rejected. It also covers every
 * configured template, the album template and aggregate templates alike, because each is a safe
 * relative name joined below `templates_dir`.
 *
 * This skips a path with no identity rather than refusing it. A missing configuration file, source
 * or asset cannot be overwritten, and neither can a missing `templates_dir`, which claims nothing.
 * `cmd_build` already rejects a missing `templates_dir` while loading the build inputs, and the
 * render reports a missing template with a template-specific diagnostic, so failing here would only
 * report an absence earlier and with less context.
 *
 * The template tree is listed first, so the set is allocated once with a slot for every candidate.
 *
 * @param inputs         Empty identity set that receives one entry per readable input file. Must
 *                       not be `NULL`.
 * @param gallery_config Configuration supplying `templates_dir`. Must not be `NULL`.
 * @param config_path    Path the configuration was loaded from. Must not be `NULL`.
 * @param source_paths   Every discovered media source path. Must not be `NULL`.
 * @param asset_paths    Every enumerated file below `assets_dir`. Must not be `NULL`.
 * @param err            Destination buffer for a failure diagnostic.
 * @param err_len        Size of `err` in bytes.
 * @return `0` when every readable input was claimed, or `-1` when the template tree cannot be
 *         walked or on allocation failure.
 */
static int claim_build_inputs(struct InputIdentities* inputs,
                              const struct GalleryConfig* gallery_config,
                              const char* config_path,
                              const struct PathList* source_paths,
                              const struct PathList* asset_paths,
                              char* err,
                              size_t err_len) __attribute__((nonnull(1, 2, 3, 4, 5)));

/**
 * @brief Lists every file below `templates_dir`, partials included.
 *
 * @param template_paths Initialized, empty path list that receives the files. Must not be `NULL`.
 * @param templates_dir  Template directory root to walk. A path with no identity lists nothing.
 *                       Must not be `NULL`.
 * @param output_dir     Output directory the walk leaves out, so a generated file reached through a
 *                       symlink is not claimed as a template. Must not be `NULL`.
 * @param err            Destination buffer for a failure diagnostic.
 * @param err_len        Size of `err` in bytes.
 * @return `0` when every file in the tree was listed or `templates_dir` has no identity, or `-1`
 *         when the walk fails.
 */
static int claim_build_inputs_list_templates(struct PathList* template_paths,
                                             const char* templates_dir,
                                             const char* output_dir,
                                             char* err,
                                             size_t err_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Claims one path's identity, ignoring a path that has none.
 *
 * @param inputs    Identity set to append to, with a free slot. Must not be `NULL`.
 * @param file_path Path whose identity is claimed. Must not be `NULL`.
 */
static void claim_input_identity(struct InputIdentities* inputs, const char* file_path)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two identities by device and then inode, for `qsort` and `bsearch`.
 *
 * @param a First `struct FsIdentity`. Must not be `NULL`.
 * @param b Second `struct FsIdentity`. Must not be `NULL`.
 * @return A negative value, zero, or a positive value as `a` orders before, equal to, or after `b`.
 */
static int compare_identities(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Releases the identity set and leaves it initialized for reuse.
 *
 * @param inputs Identity set to release. Must not be `NULL`.
 */
static void free_input_identities(struct InputIdentities* inputs) __attribute__((nonnull(1)));

/**
 * @brief Records the identity of `input_dir`, `templates_dir`, and `assets_dir`.
 *
 * A root with no identity is recorded as absent rather than refused, for the same reason
 * `claim_input_identity` skips a missing file: it holds no input an output could land among.
 *
 * @param roots          Root set to fill. Must not be `NULL`.
 * @param gallery_config Configuration supplying the roots and `output_dir`. Must not be `NULL`.
 */
static void claim_input_roots(struct InputRoots* roots, const struct GalleryConfig* gallery_config)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Finds the input root that `output_dir` is at or below, whether or not it exists yet.
 *
 * Starts from the nearest existing ancestor of `output_dir`, which is `output_dir` itself once it
 * exists, so the answer is the same before and after the build creates it. From there the walk
 * appends `..` rather than trimming the text, because the path can be relative, hold `..` segments,
 * or pass through a symlink, and the kernel resolves `..` against the directory actually reached.
 * It stops at a root match, at a path with no identity, or at the filesystem root, whose `..` is
 * itself.
 *
 * @param roots          Input directory trees to look for. Must not be `NULL`.
 * @param scratch        Arena that owns the ancestor paths. Must not be `NULL`.
 * @param config_key_out Receives the config key of the root holding `output_dir`, or `NULL` when no
 *                       root does. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int find_input_root_holding_output_dir(const struct InputRoots* roots,
                                              struct Arena* scratch,
                                              const char** config_key_out)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Returns the nearest existing ancestor of `dir_path`, which is `dir_path` itself when it
 *        exists.
 *
 * Each component is joined onto the nearest path known to exist, so an existing component, `..`
 * included, resolves the way the kernel resolves it. Past the first missing component the
 * components are only counted, because `fs_mkdir_p` creates each of them as a real directory: a
 * `..` there cancels the missing component before it, and probing resumes once none is left. That
 * keeps `missing/../photos/new` from hiding that it lands in `photos`.
 *
 * @param dir_path Directory path, absolute or relative to the working directory. Must not be
 *                 `NULL`.
 * @param scratch  Arena that owns the returned path and the copies built on the way. Must not be
 *                 `NULL`.
 * @return The nearest existing ancestor, owned by `scratch` or static, or `NULL` on allocation
 *         failure.
 */
static const char* find_nearest_existing_ancestor(const char* dir_path, struct Arena* scratch)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the config key of the input root whose identity is `identity`.
 *
 * @param roots    Root set to search. Must not be `NULL`.
 * @param identity Directory identity to look for. Must not be `NULL`.
 * @return The matching root's config key, or `NULL` when `identity` names no root.
 */
static const char* find_input_root(const struct InputRoots* roots,
                                   const struct FsIdentity* identity)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the path of an album's source directory, the label its page output is claimed
 *        under.
 *
 * The label is a quoted value in every collision diagnostic, so it has to be a real path rather
 * than a placeholder. The root album's `source_dir` is empty, so the root is labeled by `input_dir`
 * itself, and every other album by its `source_dir` below it, the same way a media item is labeled
 * by its full source path. No sub-album label can then read the same as the root's.
 *
 * @param album     Album whose source directory is returned. Must not be `NULL`.
 * @param input_dir Configured source root that `album->source_dir` is relative to. Must not be
 *                  `NULL`.
 * @param scratch   Arena that owns a joined path. Must not be `NULL`.
 * @return `input_dir` for the root album, otherwise a joined path owned by `scratch`, or `NULL` on
 *         allocation failure.
 */
static const char* album_source_path(const struct Album* album,
                                     const char* input_dir,
                                     struct Arena* scratch) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Registers one configured template's output path in the build manifest.
 *
 * @param manifest      Manifest that receives the output path. Must not be `NULL`.
 * @param output_dir    Output directory the template name is rooted under. Must not be `NULL`.
 * @param template_name Safe relative template name whose output path is registered. Must not be
 *                      `NULL`.
 * @param source_label  Diagnostic label for the configured list entry naming `template_name`, as
 *                      `<config key>[<index>]`. A bare template name would report one name listed
 *                      twice in one list as a collision between a value and itself. Must not be
 *                      `NULL`.
 * @param inputs        Identities of this build's input files, which the output must not name. Must
 *                      not be `NULL`.
 * @param roots         Input directory trees the output must not land in. Must not be `NULL`.
 * @param scratch       Arena that owns the joined output path during registration. Must not be
 *                      `NULL`.
 * @param err           Destination buffer for a failure diagnostic.
 * @param err_len       Size of `err` in bytes.
 * @return `0` on success, or `-1` on an oversize path, a collision, or allocation failure.
 */
static int register_template_output(struct Manifest* manifest,
                                    const char* output_dir,
                                    const char* template_name,
                                    const char* source_label,
                                    const struct InputIdentities* inputs,
                                    const struct InputRoots* roots,
                                    struct Arena* scratch,
                                    char* err,
                                    size_t err_len) __attribute__((nonnull(1, 2, 3, 4, 5, 6, 7)));

/**
 * @brief Registers one enumerated asset's output path in the build manifest.
 *
 * @param manifest       Manifest that receives the output path. Must not be `NULL`.
 * @param output_dir     Output directory the asset's `assets/` destination is rooted under. Must
 *                       not be `NULL`.
 * @param asset_relative Asset path relative to `assets_dir`. Must not be `NULL`.
 * @param asset_path     Asset source path, the diagnostic label for the output. Must not be `NULL`.
 * @param inputs         Identities of this build's input files, which the output must not name.
 *                       Must not be `NULL`.
 * @param roots          Input directory trees the output must not land in. Must not be `NULL`.
 * @param scratch        Arena that owns the joined output path during registration. Must not be
 *                       `NULL`.
 * @param err            Destination buffer for a failure diagnostic.
 * @param err_len        Size of `err` in bytes.
 * @return `0` on success, or `-1` on an oversize path, a collision, or allocation failure.
 */
static int register_asset_output(struct Manifest* manifest,
                                 const char* output_dir,
                                 const char* asset_relative,
                                 const char* asset_path,
                                 const struct InputIdentities* inputs,
                                 const struct InputRoots* roots,
                                 struct Arena* scratch,
                                 char* err,
                                 size_t err_len) __attribute__((nonnull(1, 2, 3, 4, 5, 6, 7)));

/**
 * @brief Adds one output path to the manifest, reporting a duplicate, an output inside an input
 *        root, an input overwrite, or an allocation failure.
 *
 * This is the one function every intended output path passes through, so the input checks live here
 * rather than in each producer.
 *
 * @param manifest     Manifest to append to. Must not be `NULL`.
 * @param output_path  Filesystem output path to record, joined onto `roots->output_dir`. Must not
 *                     be `NULL`.
 * @param source_label Diagnostic label for the source producing `output_path`: an album's source
 *                     directory path, a media item's source path, a configured template list entry,
 *                     or an asset's source path. Must not be `NULL`.
 * @param inputs       Identities of this build's input files, which `output_path` must not name.
 *                     Must not be `NULL`.
 * @param roots        Input directory trees `output_path` must not land in. Must not be `NULL`.
 * @param scratch      Arena that owns the copy of `output_path` the root check truncates. Must not
 *                     be `NULL`.
 * @param err          Destination buffer for a failure diagnostic.
 * @param err_len      Size of `err` in bytes.
 * @return `0` when the path was recorded, or `-1` when it lands in an input root, duplicates an
 *         earlier output, names a build input, or could not be recorded.
 */
static int register_output_path(struct Manifest* manifest,
                                const char* output_path,
                                const char* source_label,
                                const struct InputIdentities* inputs,
                                const struct InputRoots* roots,
                                struct Arena* scratch,
                                char* err,
                                size_t err_len) __attribute__((nonnull(1, 2, 3, 4, 5, 6)));

/**
 * @brief Finds the input root that an output path lands at or below.
 *
 * Checks each existing directory below `output_dir` down to the output path itself, so a symlink
 * inside `output_dir` that points into an input root is caught. `output_dir` and its ancestors are
 * not checked here, because `manifest_builder_populate` has already cleared them through
 * `manifest_builder_check_output_dir`. The walk stops at the first missing directory, because the
 * build creates that directory and everything below it fresh.
 *
 * @param roots          Input directory trees to look for. Must not be `NULL`.
 * @param output_path    Output path joined onto `roots->output_dir`. Must not be `NULL`.
 * @param scratch        Arena that owns the truncated copy of `output_path`. Must not be `NULL`.
 * @param config_key_out Receives the config key of the root holding `output_path`, or `NULL` when
 *                       no root does. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int find_input_root_holding(const struct InputRoots* roots,
                                   const char* output_path,
                                   struct Arena* scratch,
                                   const char** config_key_out)
    __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Reports whether `identity` names a file this build reads.
 *
 * @param inputs   Identity set to search, sorted by `claim_build_inputs`. Must not be `NULL`.
 * @param identity Identity to look for. Must not be `NULL`.
 * @return `true` when the identity was claimed as an input, `false` otherwise.
 */
static bool has_input_identity(const struct InputIdentities* inputs,
                               const struct FsIdentity* identity) __attribute__((nonnull(1, 2)));

int manifest_builder_check_output_dir(const struct GalleryConfig* gallery_config,
                                      char* err,
                                      size_t err_len) {
  struct Arena scratch;
  arena_init(&scratch);
  struct InputRoots roots;
  claim_input_roots(&roots, gallery_config);
  const char* root_key = NULL;
  int rc = find_input_root_holding_output_dir(&roots, &scratch, &root_key);
  if (rc != 0) {
    (void)error_report(err, err_len, "out of memory checking output directory");
  } else if (root_key != NULL) {
    // The root is a config key and bounded, so it stays in the sentence. The configured path
    // trails, because it is unbounded and the rule it broke is the actionable part.
    rc = error_report(err, err_len, "output directory would write inside '%s': '%s'", root_key,
                      gallery_config->output_dir);
  }
  arena_free(&scratch);
  return rc;
}

int manifest_builder_populate(struct Manifest* manifest,
                              const struct GalleryConfig* gallery_config,
                              const char* config_path,
                              const struct PathList* source_paths,
                              const struct PathList* asset_paths,
                              const struct Album* const* albums,
                              size_t album_count,
                              char* err,
                              size_t err_len) {
  struct Arena scratch;
  arena_init(&scratch);
  struct InputIdentities inputs = {0};
  struct InputRoots roots;

  // The build already ran this check before walking any input tree. Running it again here keeps the
  // protection from depending on the caller, and costs a few `fs_identify` calls.
  int rc = manifest_builder_check_output_dir(gallery_config, err, err_len);
  claim_input_roots(&roots, gallery_config);
  if (rc == 0) {
    rc = claim_build_inputs(&inputs, gallery_config, config_path, source_paths, asset_paths, err,
                            err_len);
  }

  for (size_t i = 0; rc == 0 && i < album_count; i++) {
    const struct Album* album = albums[i];
    const char* album_label = album_source_path(album, gallery_config->input_dir, &scratch);
    if (album_label == NULL) {
      rc = error_report(err, err_len, "out of memory building source path for album '%s'",
                        album->source_dir);
      continue;
    }
    rc = register_output_path(manifest, album->output_path, album_label, &inputs, &roots, &scratch,
                              err, err_len);
    for (size_t j = 0; rc == 0 && j < album->media_count; j++) {
      const struct MediaItem* media = album->media[j];
      for (size_t k = 0; rc == 0 && k < media->derivative_count; k++) {
        rc = register_output_path(manifest, media->derivatives[k].path, media->source_path, &inputs,
                                  &roots, &scratch, err, err_len);
      }
      if (rc == 0) {
        rc = register_output_path(manifest, media->original_path, media->source_path, &inputs,
                                  &roots, &scratch, err, err_len);
      }
    }
  }

  for (size_t i = 0; rc == 0 && i < gallery_config->aggregate_template_count; i++) {
    char source_label[TEMPLATE_SOURCE_LABEL_SIZE];
    (void)snprintf(source_label, sizeof(source_label), "aggregate_templates[%zu]", i);
    rc = register_template_output(manifest, gallery_config->output_dir,
                                  gallery_config->aggregate_templates[i], source_label, &inputs,
                                  &roots, &scratch, err, err_len);
  }

  for (size_t i = 0; rc == 0 && i < asset_paths->count; i++) {
    const char* relative = path_relative_below(asset_paths->items[i], gallery_config->assets_dir);
    if (relative == NULL) {
      rc = error_report(err, err_len, "asset path is not below configured assets directory: '%s'",
                        asset_paths->items[i]);
      continue;
    }
    rc = register_asset_output(manifest, gallery_config->output_dir, relative,
                               asset_paths->items[i], &inputs, &roots, &scratch, err, err_len);
  }

  // Every path is now recorded, so scan for the collision `manifest_add` cannot see incrementally:
  // one output path that is a directory prefix of another. The two labels lead the diagnostic as
  // the actionable part. The offending path trails because it is bounded only by the output-path
  // limit plus the output directory, so leading with it could truncate the labels.
  if (rc == 0) {
    struct ManifestPrefixCollision collision;
    if (manifest_find_prefix_collision(manifest, &collision)) {
      rc = error_report(err, err_len, "output path for '%s' nests under output path for '%s': '%s'",
                        collision.descendant_label, collision.ancestor_label,
                        collision.descendant_path);
    }
  }

  free_input_identities(&inputs);
  arena_free(&scratch);
  return rc;
}

char* manifest_builder_derive_template_output(const char* output_dir,
                                              const char* template_name,
                                              struct Arena* arena) {
  return path_join(output_dir, template_name, arena);
}

char* manifest_builder_derive_asset_output(const char* output_dir,
                                           const char* asset_relative,
                                           struct Arena* arena) {
  const char* output_assets_dir = path_join(output_dir, "assets", arena);
  if (output_assets_dir == NULL) {
    return NULL;
  }
  return path_join(output_assets_dir, asset_relative, arena);
}

static int claim_build_inputs(struct InputIdentities* inputs,
                              const struct GalleryConfig* gallery_config,
                              const char* config_path,
                              const struct PathList* source_paths,
                              const struct PathList* asset_paths,
                              char* err,
                              size_t err_len) {
  struct PathList template_paths;
  path_list_init(&template_paths);
  int rc = claim_build_inputs_list_templates(&template_paths, gallery_config->templates_dir,
                                             gallery_config->output_dir, err, err_len);
  if (rc == 0) {
    // One slot for the configuration file, then one per source, per asset and per template file.
    // `calloc` fails on product overflow rather than wrapping.
    inputs->items = calloc(1 + source_paths->count + asset_paths->count + template_paths.count,
                           sizeof(*inputs->items));
    if (inputs->items == NULL) {
      rc = error_report(err, err_len, "out of memory recording build input files");
    } else {
      claim_input_identity(inputs, config_path);
      for (size_t i = 0; i < source_paths->count; i++) {
        claim_input_identity(inputs, source_paths->items[i]);
      }
      for (size_t i = 0; i < asset_paths->count; i++) {
        claim_input_identity(inputs, asset_paths->items[i]);
      }
      for (size_t i = 0; i < template_paths.count; i++) {
        claim_input_identity(inputs, template_paths.items[i]);
      }
      qsort(inputs->items, inputs->count, sizeof(*inputs->items), compare_identities);
    }
  }
  path_list_free(&template_paths);
  return rc;
}

static int claim_build_inputs_list_templates(struct PathList* template_paths,
                                             const char* templates_dir,
                                             const char* output_dir,
                                             char* err,
                                             size_t err_len) {
  // A `templates_dir` with no identity holds no file to overwrite, so there is nothing to claim.
  // This is the same skip `claim_input_identity` applies to a single missing path. A full build
  // never gets here with one, because `cmd_build` rejects a missing `templates_dir` while loading
  // the build inputs, so reporting it is left to that earlier check. An existing root that cannot
  // be walked still fails, because a partial the walk could not see would stay overwritable.
  struct FsIdentity templates_dir_identity;
  if (fs_identify(templates_dir, &templates_dir_identity) != 0) {
    return 0;
  }
  // An empty suffix matches every filename, so the walk lists the whole tree.
  static const char* const all_suffixes[] = {""};
  char reason[FS_REASON_SIZE];
  if (fs_list_files_with_suffixes(template_paths, templates_dir, output_dir, all_suffixes, 1, false,
                                  reason, sizeof(reason)) != 0) {
    // The reason names the directory or entry that failed, which is more precise than the
    // configured root, so the root is not repeated here.
    return error_report(err, err_len, "failed to list template files: %s", reason);
  }
  return 0;
}

static void claim_input_identity(struct InputIdentities* inputs, const char* file_path) {
  struct FsIdentity identity;
  if (fs_identify(file_path, &identity) == 0) {
    inputs->items[inputs->count++] = identity;
  }
}

static int compare_identities(const void* a, const void* b) {
  const struct FsIdentity* left = a;
  const struct FsIdentity* right = b;
  if (left->device != right->device) {
    return left->device < right->device ? -1 : 1;
  }
  if (left->inode != right->inode) {
    return left->inode < right->inode ? -1 : 1;
  }
  return 0;
}

static void free_input_identities(struct InputIdentities* inputs) {
  free(inputs->items);
  *inputs = (struct InputIdentities){0};
}

static void claim_input_roots(struct InputRoots* roots,
                              const struct GalleryConfig* gallery_config) {
  *roots = (struct InputRoots){
      .items = {{.config_key = "input_dir"},
                {.config_key = "templates_dir"},
                {.config_key = "assets_dir"}},
      .output_dir = gallery_config->output_dir,
  };
  const char* root_dirs[INPUT_ROOT_COUNT] = {
      gallery_config->input_dir, gallery_config->templates_dir, gallery_config->assets_dir};
  for (size_t i = 0; i < INPUT_ROOT_COUNT; i++) {
    roots->items[i].is_present = fs_identify(root_dirs[i], &roots->items[i].identity) == 0;
  }
}

static int find_input_root_holding_output_dir(const struct InputRoots* roots,
                                              struct Arena* scratch,
                                              const char** config_key_out) {
  *config_key_out = NULL;
  const char* dir_path = find_nearest_existing_ancestor(roots->output_dir, scratch);
  if (dir_path == NULL) {
    return -1;
  }
  struct FsIdentity identity;
  if (fs_identify(dir_path, &identity) != 0) {
    return 0;
  }
  for (;;) {
    *config_key_out = find_input_root(roots, &identity);
    if (*config_key_out != NULL) {
      return 0;
    }
    dir_path = path_join(dir_path, "..", scratch);
    if (dir_path == NULL) {
      return -1;
    }
    struct FsIdentity parent_identity;
    if (fs_identify(dir_path, &parent_identity) != 0 ||
        (parent_identity.device == identity.device && parent_identity.inode == identity.inode)) {
      return 0;
    }
    identity = parent_identity;
  }
}

static const char* find_nearest_existing_ancestor(const char* dir_path, struct Arena* scratch) {
  char* components = arena_strdup(scratch, dir_path);
  if (components == NULL) {
    return NULL;
  }
  const char* ancestor_path = dir_path[0] == '/' ? "/" : ".";
  size_t missing_count = 0;
  char* cursor = components;
  while (*cursor != '\0') {
    char* component = cursor;
    cursor += strcspn(cursor, "/");
    if (*cursor == '/') {
      *cursor = '\0';
      cursor++;
    }
    if (component[0] == '\0' || strcmp(component, ".") == 0) {
      continue;
    }
    if (missing_count > 0) {
      missing_count = strcmp(component, "..") == 0 ? missing_count - 1 : missing_count + 1;
      continue;
    }
    const char* candidate_path = path_join(ancestor_path, component, scratch);
    if (candidate_path == NULL) {
      return NULL;
    }
    struct FsIdentity identity;
    if (fs_identify(candidate_path, &identity) == 0) {
      ancestor_path = candidate_path;
    } else {
      missing_count = 1;
    }
  }
  return ancestor_path;
}

static const char* find_input_root(const struct InputRoots* roots,
                                   const struct FsIdentity* identity) {
  for (size_t i = 0; i < INPUT_ROOT_COUNT; i++) {
    const struct InputRoot* root = &roots->items[i];
    if (root->is_present && root->identity.device == identity->device &&
        root->identity.inode == identity->inode) {
      return root->config_key;
    }
  }
  return NULL;
}

static const char* album_source_path(const struct Album* album,
                                     const char* input_dir,
                                     struct Arena* scratch) {
  if (album_is_root(album)) {
    return input_dir;
  }
  return path_join(input_dir, album->source_dir, scratch);
}

static int register_template_output(struct Manifest* manifest,
                                    const char* output_dir,
                                    const char* template_name,
                                    const char* source_label,
                                    const struct InputIdentities* inputs,
                                    const struct InputRoots* roots,
                                    struct Arena* scratch,
                                    char* err,
                                    size_t err_len) {
  // This is checked against the same limits as an album or media output path. All are generated
  // output paths joined onto `output_dir`, and all land in this manifest. `album_scanner` checks
  // the paths it generates, so no producer reaches the manifest with an unbounded output path.
  if (output_path_check_limits(template_name, source_label, err, err_len) != 0) {
    return -1;
  }

  const char* output_path =
      manifest_builder_derive_template_output(output_dir, template_name, scratch);
  if (output_path == NULL) {
    return error_report(err, err_len, "out of memory building output path for '%s'", template_name);
  }
  return register_output_path(manifest, output_path, source_label, inputs, roots, scratch, err,
                              err_len);
}

static int register_asset_output(struct Manifest* manifest,
                                 const char* output_dir,
                                 const char* asset_relative,
                                 const char* asset_path,
                                 const struct InputIdentities* inputs,
                                 const struct InputRoots* roots,
                                 struct Arena* scratch,
                                 char* err,
                                 size_t err_len) {
  const char* output_path =
      manifest_builder_derive_asset_output(output_dir, asset_relative, scratch);
  if (output_path == NULL) {
    return error_report(err, err_len, "out of memory building output path for asset '%s'",
                        asset_path);
  }
  // The limits bound the path below `output_dir`. That is the derived path past `output_dir` and
  // the separator `path_join` adds, the same offset `find_input_root_holding` starts from.
  if (output_path_check_limits(output_path + strlen(output_dir) + 1, asset_path, err, err_len) !=
      0) {
    return -1;
  }
  return register_output_path(manifest, output_path, asset_path, inputs, roots, scratch, err,
                              err_len);
}

static int register_output_path(struct Manifest* manifest,
                                const char* output_path,
                                const char* source_label,
                                const struct InputIdentities* inputs,
                                const struct InputRoots* roots,
                                struct Arena* scratch,
                                char* err,
                                size_t err_len) {
  // Both input checks run before the manifest, because they catch the failures that corrupt this
  // build's inputs rather than merely abandoning the build. The root check runs first because it is
  // the broader rule. It rejects every output below an input root, whether or not a file already
  // sits at that path.
  const char* root_key = NULL;
  if (find_input_root_holding(roots, output_path, scratch, &root_key) != 0) {
    return error_report(err, err_len, "out of memory checking output path for '%s'", source_label);
  }
  if (root_key != NULL) {
    // The root is a config key and bounded, so it stays in the sentence. The path trails for the
    // same reason as in the overwrite message below.
    return error_report(err, err_len, "output path would write inside '%s' for '%s': '%s'",
                        root_key, source_label, output_path);
  }

  // An output path naming one of this build's own input files would replace that file with
  // generated output, and nothing later in the build would notice. `fs_identify` failing means the
  // path names nothing yet, which is the ordinary case for an output about to be created.
  struct FsIdentity identity;
  if (fs_identify(output_path, &identity) == 0 && has_input_identity(inputs, &identity)) {
    // The producer is the actionable part and the path trails it, matching the duplicate message
    // below. The path is bounded only by `OUTPUT_PATH_RELATIVE_LEN_MAX` plus the output directory,
    // so leading with it would truncate the label away.
    return error_report(err, err_len, "output path would overwrite build input for '%s': '%s'",
                        source_label, output_path);
  }

  const char* source_label_existing = NULL;
  switch (manifest_add(manifest, output_path, source_label, &source_label_existing)) {
    case MANIFEST_ADD_INSERTED:
      return 0;
    case MANIFEST_ADD_DUPLICATE:
      // The two colliding producers are the actionable part and the path trails them. It is bounded
      // only by `OUTPUT_PATH_RELATIVE_LEN_MAX` plus the output directory, which is twice
      // `ERROR_MESSAGE_SIZE`, and leading with it would truncate both labels away. The path is this
      // claim's spelling, which may differ from the earlier claim's in ASCII case alone, because
      // `manifest_add` folds case to catch outputs a case-insensitive filesystem would merge.
      return error_report(err, err_len, "duplicate output path for '%s' and '%s': '%s'",
                          source_label_existing, source_label, output_path);
    case MANIFEST_ADD_ERROR:
      return error_report(err, err_len, "out of memory recording output path '%s'", output_path);
  }
  // Unreachable: the switch covers every `enum ManifestAdd`. This is deliberately not a `default:`
  // case, so `-Wswitch` still fails the build when someone adds an outcome without a case here. A
  // value outside the enum means memory corruption, not a failure this function can report.
  abort();
}

static int find_input_root_holding(const struct InputRoots* roots,
                                   const char* output_path,
                                   struct Arena* scratch,
                                   const char** config_key_out) {
  *config_key_out = NULL;
  // Each directory below `output_dir` is named by cutting a copy of the path at a `/`. Starting
  // past `output_dir` and its separator keeps the walk off `output_dir`'s own text, whose prefixes
  // need not be its ancestors once it holds `..`. The final pass checks the output path itself, so
  // an output that names a root directory outright is caught too.
  char* dir_path = arena_strdup(scratch, output_path);
  if (dir_path == NULL) {
    return -1;
  }
  for (char* cursor = dir_path + strlen(roots->output_dir) + 1;; cursor++) {
    const char c = *cursor;
    if (c != '/' && c != '\0') {
      continue;
    }
    *cursor = '\0';
    struct FsIdentity identity;
    const bool is_present = fs_identify(dir_path, &identity) == 0;
    *cursor = c;
    if (!is_present) {
      return 0;
    }
    *config_key_out = find_input_root(roots, &identity);
    if (*config_key_out != NULL || c == '\0') {
      return 0;
    }
  }
}

static bool has_input_identity(const struct InputIdentities* inputs,
                               const struct FsIdentity* identity) {
  return inputs->count > 0 && bsearch(identity, inputs->items, inputs->count,
                                      sizeof(*inputs->items), compare_identities) != NULL;
}
