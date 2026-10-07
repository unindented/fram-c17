#ifndef FRAM_GALLERY_CONFIG_H
#define FRAM_GALLERY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "shared/arena.h"

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
extern const char* const GALLERY_CONFIG_PATH_DEFAULT;

/** Stable derivative recipe version. Increment when output bytes must be regenerated. */
enum { DERIVATIVE_RECIPE_VERSION = 1 };

/** Largest accepted derivative-name length, excluding the terminating `NUL`. */
enum { GALLERY_DERIVATIVE_NAME_LEN_MAX = 15 };

/** Largest accepted number of configured derivative recipes. */
enum { GALLERY_DERIVATIVE_COUNT_MAX = 6 };

/** One named image derivative generated for every media item. */
struct GalleryDerivative {
  /** Stable identifier used in generated paths and template data. */
  const char* name;

  /** Output bounding box in pixels. */
  size_t width_px;
  size_t height_px;

  /** JPEG quality in the range 1 through 100. */
  size_t quality;

  /** Whether the source is center-cropped to the bounding-box aspect ratio before resizing. */
  bool is_crop;
};

/** Gallery configuration loaded from `fram.toml`. */
struct GalleryConfig {
  /** Owns strings and arrays copied from configuration. */
  struct Arena arena;

  /** Required gallery title used by templates, also the root album's title. */
  const char* title;

  /** Required author name used by templates. */
  const char* author;

  /** Optional absolute deployment URL, or an empty string when absent. */
  const char* base_url;

  /** Directory containing source media. Defaults to `media`. */
  const char* input_dir;

  /** Directory where generated files are written. Defaults to `public`. */
  const char* output_dir;

  /** Directory containing templates. Defaults to `templates`. */
  const char* templates_dir;

  /** Directory containing static files copied as-is into `output_dir`. Defaults to `static`. */
  const char* static_dir;

  /** Safe relative template name for every album page. Defaults to `album.html`. */
  const char* album_template;

  /**
   * Safe relative template names rendered as aggregate outputs. Defaults to `[]`. It is deeply
   * `const` as a thread-safety invariant. Render workers reach this config through
   * `TemplateContext`, and the default arrays only land in read-only data while their elements are
   * `const` too. Do not relax to `const char**`.
   */
  const char* const* aggregate_templates;

  /** Number of names in `aggregate_templates`. */
  size_t aggregate_template_count;

  /** Named derivative recipes, in configuration order. */
  const struct GalleryDerivative* derivatives;

  /** Number of recipes in `derivatives`, from one to `GALLERY_DERIVATIVE_COUNT_MAX`. */
  size_t derivative_count;

  /** Preferred video-frame timestamp in seconds. Defaults to `1`. */
  size_t video_frame_seconds;
};

/**
 * @brief Initializes a gallery config with defaults for optional configuration keys.
 *
 * Prepares the config's arena and sets defaults for optional keys, leaving required keys unset for
 * `gallery_config_load` to fill.
 *
 * @param gallery_config Config handle to prepare. Must not be `NULL` or already own allocations.
 */
void gallery_config_init(struct GalleryConfig* gallery_config) __attribute__((nonnull(1)));

/**
 * @brief Releases arena-owned config data and resets it for reuse.
 *
 * Invalidates every arena-owned pointer on the config. The config stays initialized, so it may be
 * reused without calling `gallery_config_init`.
 *
 * @param gallery_config Config to release. Must not be `NULL`.
 */
void gallery_config_free(struct GalleryConfig* gallery_config) __attribute__((nonnull(1)));

/**
 * @brief Loads `fram.toml` configuration into `gallery_config`.
 *
 * Reads and parses the file, rejects a key outside the schema, validates required keys, template
 * names, and derivative recipes, and copies values into the config's arena over the defaults from
 * `gallery_config_init`.
 *
 * @param gallery_config Initialized config that receives the loaded values. Must not be `NULL`.
 * @param config_path    Path to the TOML configuration file. Must not be `NULL`.
 * @param err            Buffer for a diagnostic message on failure.
 * @param err_len        Size of `err` in bytes.
 * @return `0` on success, or `-1` on a read, parse, allocation, or validation error, with a
 *         diagnostic in `err`.
 */
int gallery_config_load(struct GalleryConfig* gallery_config,
                        const char* config_path,
                        char* err,
                        size_t err_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the effective configuration to `stream` as valid TOML.
 *
 * The output round-trips. Reloading it through `gallery_config_load` yields the same values.
 *
 * @param stream         Destination stream. Must not be `NULL`.
 * @param gallery_config Config to serialize. Must be fully populated (as after a successful
 *                       `gallery_config_load`), with `title` and `author` non-`NULL`. Must not be
 *                       `NULL`.
 * @return `0` on success, or `-1` if writing to `stream` failed, with `errno` set by the failing
 *         write, or to `EIO` when the stream had latched an error earlier and the original `errno`
 *         is no longer available, so the caller always has a reason to report.
 */
int gallery_config_print(FILE* stream, const struct GalleryConfig* gallery_config)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Builds the stable path component identifying a derivative recipe.
 *
 * @param gallery_config Config whose output-affecting values identify the recipe. Must not be
 *                       `NULL`.
 * @param arena          Arena that owns the returned component. Must not be `NULL`.
 * @return Recipe component owned by `arena`, or `NULL` on formatting or allocation failure.
 */
char* gallery_config_derivative_recipe(const struct GalleryConfig* gallery_config,
                                       struct Arena* arena) __attribute__((nonnull(1, 2)));

#endif
