#ifndef FRAM_GALLERY_CONFIG_H
#define FRAM_GALLERY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct Log;
struct Os;

/**
 * Path every command loads its gallery configuration from, relative to the working directory.
 *
 * It is declared here rather than inside a command, because two commands need it and must agree on
 * it. `cmd_config_run` reads it, and `cmd_build_run` both reads it and claims it as a build input,
 * so no output path can overwrite it. `gallery_config_load` still takes the path as an argument,
 * because tests load fixtures from elsewhere. This is the default the commands pass, not a bound on
 * the loader.
 *
 * It is `extern`, with the definition in `gallery_config.c`, rather than a `static` initializer in
 * this header. Every translation unit then shares one object instead of getting a copy of its own
 * that most of them never read.
 */
extern const struct Str GALLERY_CONFIG_PATH_DEFAULT;

/** Stable derivative recipe version. Increment when output bytes must be regenerated. */
enum { DERIVATIVE_RECIPE_VERSION = 1 };

/** Largest accepted derivative-name length, excluding the terminating `NUL`. */
enum { GALLERY_DERIVATIVE_NAME_LEN_MAX = 15 };

/** Largest accepted number of configured derivative recipes. */
enum { GALLERY_DERIVATIVE_COUNT_MAX = 6 };

/** One named image derivative generated for every media item. */
struct GalleryDerivative {
  /** Stable identifier used in generated paths and template data. */
  struct Str name;

  /** Output bounding box in pixels. */
  ptrdiff_t width_px;
  ptrdiff_t height_px;

  /** JPEG quality in the range 1 through 100. */
  ptrdiff_t quality;

  /** Whether the source is center-cropped to the bounding-box aspect ratio before resizing. */
  bool is_crop;
};

/** Direction in which an album lists its child albums or its media. */
enum GallerySortOrder {
  /** Ascending source filename, the default. */
  GALLERY_SORT_ASC,

  /** Descending source filename. */
  GALLERY_SORT_DESC,
};

/**
 * Gallery configuration loaded from `fram.toml`. Every string and array it points at is either a
 * default literal or a copy in the arena `gallery_config_load` was given.
 */
struct GalleryConfig {
  /** Required gallery title used by templates, also the root album's title. */
  struct Str title;

  /** Required author name used by templates. */
  struct Str author;

  /** Optional absolute deployment URL, or an empty string when absent. */
  struct Str base_url;

  /** Directory containing source media. Defaults to `media`. */
  struct Str input_dir;

  /** Directory where generated files are written. Defaults to `public`. */
  struct Str output_dir;

  /** Directory containing templates. Defaults to `templates`. */
  struct Str templates_dir;

  /** Directory containing static files copied as-is into `output_dir`. Defaults to `static`. */
  struct Str static_dir;

  /** Safe relative template name for every album page. Defaults to `album.html`. */
  struct Str album_template;

  /**
   * Safe relative template names rendered as aggregate outputs. Defaults to `[]`. It is `const` as
   * a thread-safety invariant. Render workers reach this config through `TemplateContext`, and the
   * default arrays only land in read-only data while they are `const`.
   */
  const struct Str* aggregate_templates;

  /** Number of names in `aggregate_templates`. */
  ptrdiff_t aggregate_template_count;

  /** Named derivative recipes, in configuration order. */
  const struct GalleryDerivative* derivatives;

  /** Number of recipes in `derivatives`, from one to `GALLERY_DERIVATIVE_COUNT_MAX`. */
  ptrdiff_t derivative_count;

  /** Order of each album's `sub_albums`. Defaults to `GALLERY_SORT_ASC`. */
  enum GallerySortOrder album_sort_order;

  /** Order of each album's direct `media`. Defaults to `GALLERY_SORT_ASC`. */
  enum GallerySortOrder media_sort_order;

  /** Preferred video-frame timestamp in seconds. Defaults to `1`. */
  ptrdiff_t video_frame_seconds;

  /**
   * Whether the load rejected `input_dir`, which then holds its default rather than a directory the
   * user chose.
   */
  bool is_input_dir_rejected;

  /** Whether the load rejected `output_dir`, which then holds its default. */
  bool is_output_dir_rejected;

  /** Whether the load rejected `templates_dir`, which then holds its default. */
  bool is_templates_dir_rejected;

  /** Whether the load rejected `static_dir`, which then holds its default. */
  bool is_static_dir_rejected;
};

/**
 * @brief Initializes a gallery config with defaults for optional configuration keys.
 *
 * Sets defaults for optional keys, leaving required keys unset for `gallery_config_load` to fill.
 *
 * @param gallery_config Config to prepare. Must not be `NULL`.
 */
void gallery_config_init(struct GalleryConfig* gallery_config) __attribute__((nonnull(1)));

/**
 * @brief Loads `fram.toml` configuration into `gallery_config`.
 *
 * Reads and parses the file, rejects a key outside the schema, validates required keys, template
 * names, derivative recipes, and sort orders, and copies values into `perm` over the defaults from
 * `gallery_config_init`.
 *
 * @param gallery_config Initialized config that receives the loaded values. Must not be `NULL`.
 * @param config_path    Path to the TOML configuration file.
 * @param os             Host the file is read from. Must not be `NULL`.
 * @param perm           Arena that owns the loaded values for as long as the config is used. Must
 *                       not be `NULL`.
 * @param scratch        Arena for the file bytes, which tomlc17 copies.
 * @param log            Log that receives every error. Must not be `NULL`.
 * @return `0` when the file was read and parsed, or `-1` when it could not be. A key that fails
 *         validation is logged and keeps its default, and the load goes on to check every other
 *         key, so `0` does not mean the config is valid: a caller checks `log->errors` before it
 *         relies on the values, and checks no directory whose key was rejected. That lets a build
 *         report a bad key and the errors in its media in one run, while a file that is missing or
 *         not TOML stops it, because there is nothing to check the media against.
 */
int gallery_config_load(struct GalleryConfig* gallery_config,
                        struct Str config_path,
                        struct Os* os,
                        struct Arena* perm,
                        struct Arena scratch,
                        struct Log* log) __attribute__((nonnull(1, 3, 4, 6)));

/**
 * @brief Writes the effective configuration to standard output as valid TOML.
 *
 * The output round-trips. Reloading it through `gallery_config_load` yields the same values. The
 * text is formatted whole in `scratch` and printed in one `os_print` call.
 *
 * @param gallery_config Config to serialize. Must be fully populated (as after a successful
 *                       `gallery_config_load`). Must not be `NULL`.
 * @param os             Host to print on. Must not be `NULL`.
 * @param scratch        Arena the text is formatted in.
 * @return `0` on success, or `-1` when the write fails.
 */
int gallery_config_print(const struct GalleryConfig* gallery_config,
                         struct Os* os,
                         struct Arena scratch) __attribute__((nonnull(1, 2)));

/**
 * @brief Builds the stable path component identifying a derivative recipe.
 *
 * @param gallery_config Config whose output-affecting values identify the recipe, within the limits
 *                       `gallery_config_load` enforces. Must not be `NULL`.
 * @param arena          Arena that owns the returned component. Must not be `NULL`.
 * @return Recipe component owned by `arena`.
 */
struct Str gallery_config_derivative_recipe(const struct GalleryConfig* gallery_config,
                                            struct Arena* arena) __attribute__((nonnull(1, 2)));

#endif
