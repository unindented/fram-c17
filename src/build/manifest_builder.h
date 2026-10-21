#ifndef FRAM_MANIFEST_BUILDER_H
#define FRAM_MANIFEST_BUILDER_H

#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct Album;
struct GalleryConfig;
struct Log;
struct Manifest;
struct Os;
struct PathList;

/**
 * @brief Rejects an `output_dir` at or below `input_dir`, `templates_dir`, or `static_dir`, whether
 *        or not it exists yet.
 *
 * A build runs this before it walks an input tree or creates anything. An `output_dir` inside a
 * root puts every output inside it, where the next build would read them back as inputs, so this
 * one answer covers every output at once. Each root is compared by `(device, inode)` against the
 * nearest existing ancestor of `output_dir` and each of that directory's physical ancestors, so an
 * alternate spelling of a root, a symlinked root, and a symlink on the way to `output_dir` are all
 * caught, and a sibling such as `photosx` is not. The nearest existing ancestor is `output_dir`
 * itself once it exists, so the answer does not depend on whether an earlier build created it. A
 * root that does not exist holds nothing to protect and is skipped.
 *
 * @param gallery_config Configuration supplying `input_dir`, `output_dir`, `templates_dir`, and
 *                       `static_dir`. Must not be `NULL`.
 * @param os             Host the directories are on. Must not be `NULL`.
 * @param scratch        Arena for the ancestor paths the check builds.
 * @param log            Log that receives the error. Must not be `NULL`.
 * @return `0` when no input root holds `output_dir`, or `-1` when one does.
 */
int manifest_builder_check_output_dir(const struct GalleryConfig* gallery_config,
                                      struct Os* os,
                                      struct Arena scratch,
                                      struct Log* log) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Records every intended output path in the build manifest, rejecting duplicates and any
 *        output that would land inside a build input tree or overwrite a build input file.
 *
 * Registers album page, derivative, original, aggregate, and static file output paths before
 * writing any file, so it catches a collision between two producers up front. It rejects four kinds
 * of collision:
 * - an output path at or below `input_dir`, `templates_dir`, or `static_dir`
 * - an output path that names a build input
 * - two producers that claim one output path, compared after folding ASCII case
 * - an output path that is a `/`-delimited directory prefix of another output path
 *
 * The first case catches an output that would be read back as an input without overwriting one: a
 * new file below `input_dir` can be discovered as media and one below `static_dir` is copied as a
 * static file by the next build, and a new file below `templates_dir` can be read as a partial by
 * the render of this same build. The caller has already run `manifest_builder_check_output_dir`, so
 * this compares each root by `(device, inode)` against each existing directory between `output_dir`
 * and the output. That catches a symlink inside `output_dir` that leads into a root. A root that
 * does not exist holds nothing to protect and is skipped.
 *
 * The last case requires one path to be both a file and a directory.
 * `manifest_find_prefix_collision` checks for it after all paths are recorded. Both path checks
 * fold ASCII `A-Z` to `a-z`, so `Trips/index.html` and `trips/index.html` are one output, as they
 * are on a case-insensitive filesystem such as the macOS default. Every other byte compares
 * exactly.
 *
 * The input-file check compares filesystem identity, not path text, because the two spellings need
 * not match: `output_dir = "."` with an aggregate template named `fram.toml` produces `./fram.toml`
 * against the config path `fram.toml`. Comparing `(device, inode)` also rejects a collision created
 * by a symlink, a hard link, or a case-insensitive filesystem. It claims the config, every
 * discovered source and static file, and every file below `templates_dir`, so it still protects an
 * input that a link places outside all three roots. Claiming the whole template tree covers
 * partials even though `template_render_file` resolves them lazily from names inside template
 * bytes. The template walk leaves out `output_dir`, so a generated file that a symlink inside
 * `templates_dir` reaches is not mistaken for a template. A missing `templates_dir` claims nothing,
 * because it holds no file to overwrite. A full build never gets here with one: loading the build
 * inputs already rejects it.
 *
 * @param manifest       Manifest that receives the output paths. Must not be `NULL`.
 * @param gallery_config Configuration supplying `input_dir`, `output_dir`, `templates_dir`,
 *                       `static_dir`, the album template, and the aggregate template list. Must not
 *                       be `NULL`.
 * @param config_path    Path the configuration itself was loaded from. Claimed as an input like any
 *                       other, so a build cannot overwrite the file that configured it.
 * @param source_paths   Every discovered media source path, whose files must not be overwritten.
 *                       Must not be `NULL`.
 * @param static_paths   Every enumerated file below `static_dir`, each claimed as an input and
 *                       registered at its path relative to `static_dir`, below `output_dir`. Must
 *                       not be `NULL`.
 * @param albums         Planned albums whose page, derivative, and original output paths, each
 *                       joined onto `output_dir`, are recorded. Must not be `NULL`.
 * @param album_count    Number of albums in `albums`.
 * @param os             Host the inputs and outputs are on. Must not be `NULL`.
 * @param perm           Arena that owns the manifest's entries, for as long as the manifest is
 *                       used. Must not be `NULL`.
 * @param scratch        Arena for the input walk and the per-output checks.
 * @param log            Log that receives every error. Every output is checked even after one
 *                       fails, so each collision is reported in one run. Must not be `NULL`.
 * @return `0` when every output path is unique, stays out of every input tree, overwrites no input,
 *         and nests under no other, or `-1` on an output inside an input tree, a duplicate, a
 *         prefix collision, an input overwrite, an oversize path, a static file outside
 *         `static_dir`, or a failed template walk.
 */
int manifest_builder_populate(struct Manifest* manifest,
                              const struct GalleryConfig* gallery_config,
                              struct Str config_path,
                              const struct PathList* source_paths,
                              const struct PathList* static_paths,
                              const struct Album* const* albums,
                              ptrdiff_t album_count,
                              struct Os* os,
                              struct Arena* perm,
                              struct Arena scratch,
                              struct Log* log) __attribute__((nonnull(1, 2, 4, 5, 6, 8, 9, 11)));

#endif
