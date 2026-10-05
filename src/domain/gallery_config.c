#include "domain/gallery_config.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc17.h>

#include "core/ascii.h"
#include "core/error.h"
#include "core/path.h"
#include "core/text.h"
#include "formats/image.h"
#include "formats/toml.h"
#include "runtime/fs.h"

/** String-valued gallery config key policy. */
struct GalleryConfigStringKey {
  /** TOML key name. */
  const char* key;

  /** Address of the `GalleryConfig` field this key fills. */
  const char** field;

  /** Whether absence is a load error rather than leaving the initialized default. */
  bool is_required;
};

/** Integer-valued gallery config key policy. */
struct GalleryConfigIntKey {
  /** TOML key name. */
  const char* key;

  /** Address of the `GalleryConfig` field this key fills. */
  size_t* field;

  /** Smallest accepted value, named in the `must be at least` rejection. */
  size_t value_min;

  /** Largest accepted value. A larger one is rejected as exceeding `limit_name`. */
  size_t value_max;

  /** Prose name of `value_max` in the overflow diagnostic: `exceeds max video frame seconds`. */
  const char* limit_name;

  /** Whether absence is a load error rather than leaving the field's current value. */
  bool is_required;
};

const char* const GALLERY_CONFIG_PATH_DEFAULT = "fram.toml";

static const char* const INPUT_DIR_DEFAULT = "media";
static const char* const OUTPUT_DIR_DEFAULT = "public";
static const char* const TEMPLATES_DIR_DEFAULT = "templates";
static const char* const ASSETS_DIR_DEFAULT = "assets";
static const char* const ALBUM_TEMPLATE_DEFAULT = "album.html";

// The root album already renders to `index.html`. A default aggregate of that name would make the
// default configuration fail its manifest phase, so the default array is intentionally empty.
static const char* const AGGREGATE_TEMPLATES_DEFAULT[] = {NULL};

/**
 * Every key the top-level table may hold. The load rejects a key outside this list, so a misspelled
 * key cannot silently leave its default in place. A key `gallery_config_load_fields` reads must be
 * listed here.
 */
static const char* const config_keys[] = {
    // clang-format off: one key per line, so adding or removing a key is a one-line change.
    "title",
    "author",
    "base_url",
    "input_dir",
    "output_dir",
    "templates_dir",
    "assets_dir",
    "album_template",
    "aggregate_templates",
    "derivatives",
    "video_frame_seconds",
    // clang-format on
};

/** Every key a `derivatives.<name>` table may hold, each read by `copy_derivatives`. */
static const char* const derivative_keys[] = {"width", "height", "quality", "crop"};

static const struct GalleryDerivative DERIVATIVES_DEFAULT[] = {
    {.name = "s", .width_px = 120, .height_px = 120, .quality = 70, .is_crop = true},
    {.name = "m", .width_px = 520, .height_px = 360, .quality = 80, .is_crop = true},
    {.name = "l", .width_px = 792, .height_px = 594, .quality = 85, .is_crop = false},
};

enum {
  /** Video-frame timestamp used when `video_frame_seconds` is absent from the configuration. */
  VIDEO_FRAME_SECONDS_DEFAULT = 1,

  /** Largest accepted `video_frame_seconds`, one day in seconds. */
  VIDEO_FRAME_SECONDS_MAX = 86400,

  /** Size of the stack buffer `gallery_config_derivative_recipe` formats its component into. */
  RECIPE_COMPONENT_SIZE = 256,
};

/**
 * Largest config file read, in bytes.
 *
 * A config is a few dozen keys and derivative tables, well under a kilobyte, so 1 MiB is three
 * orders of magnitude of headroom while keeping what tomlc17 copies and builds from it to a few
 * MiB. The read checks it before loading the file, so an oversize config is never resident.
 */
enum { CONFIG_FILE_LEN_MAX = 1024 * 1024 };

// `toml_parse_named` takes the document length as an `int`, and converting a `size_t` above
// `INT_MAX` to `int` is implementation-defined. Every config the read accepts must convert exactly.
_Static_assert(CONFIG_FILE_LEN_MAX <= INT_MAX,
               "an accepted config must fit the int length tomlc17 takes");

/**
 * @brief Reads and parses a TOML config file into a `toml_result_t`.
 *
 * On success `parsed_out` owns the parse result until the caller runs `toml_free`.
 *
 * @param config_path Path to the config file. Must not be `NULL`.
 * @param parsed_out  Receives the parsed TOML result on success. Must not be `NULL`.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` on a read, size, embedded `NUL`, or TOML parse error.
 */
static int gallery_config_load_toml(const char* config_path,
                                    toml_result_t* parsed_out,
                                    char* err,
                                    size_t err_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Copies recognized configuration keys into gallery config storage.
 *
 * Rejects a key outside `config_keys`, then applies required and optional string keys, the
 * template-name array, the derivative tables, and `video_frame_seconds`. It copies every value into
 * the config's arena rather than borrowing from `table`. `gallery_config_load` calls `toml_free` as
 * soon as this returns, which releases the pool every `toml_datum_t` string points into, while the
 * config outlives the parse.
 *
 * @param gallery_config Config that receives the values. Must not be `NULL`.
 * @param table          Parsed TOML top-level table.
 * @param err            Buffer for a diagnostic message on failure.
 * @param err_len        Size of `err` in bytes.
 * @return `0` on success, or `-1` on the first key that fails validation.
 */
static int gallery_config_load_fields(struct GalleryConfig* gallery_config,
                                      toml_datum_t table,
                                      char* err,
                                      size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Copies one string key into the arena, enforcing required keys.
 *
 * @param table       Parsed TOML table.
 * @param key         Key to read. Must not be `NULL`.
 * @param is_required Whether absence is an error. When `false`, absence leaves `*value_out`
 *                    untouched.
 * @param arena       Arena that owns the copied value. Must not be `NULL`.
 * @param value_out   Receives the copied string when the key is present. Must not be `NULL`.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` if a required key is missing, mistyped, or cannot be copied.
 */
static int copy_string(toml_datum_t table,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       const char** value_out,
                       char* err,
                       size_t err_len) __attribute__((nonnull(2, 4, 5)));

/**
 * @brief Copies one optional template-name array key and validates each name.
 *
 * When the key is absent, this leaves the outputs untouched and keeps the defaults from
 * `gallery_config_init`.
 *
 * @param table                   Parsed TOML table.
 * @param key                     Array key to read. Must not be `NULL`.
 * @param arena                   Arena that owns the copied names. Must not be `NULL`.
 * @param template_names_out      Receives the copied name array when the key is present. Must not
 *                                be `NULL`.
 * @param template_name_count_out Receives the number of names when the key is present. Must not be
 *                                `NULL`.
 * @param err                     Buffer for a diagnostic message on failure.
 * @param err_len                 Size of `err` in bytes.
 * @return `0` on success or when absent, or `-1` on a wrong type, unsafe name, or allocation
 *         failure.
 */
static int copy_template_names(toml_datum_t table,
                               const char* key,
                               struct Arena* arena,
                               const char* const** template_names_out,
                               size_t* template_name_count_out,
                               char* err,
                               size_t err_len) __attribute__((nonnull(2, 3, 4, 5)));

/**
 * @brief Copies and validates the optional named-derivative table.
 *
 * When the key is absent, this leaves `derivatives` untouched and keeps the defaults from
 * `gallery_config_init`.
 *
 * @param gallery_config Config that receives the derivatives. Must not be `NULL`.
 * @param table          Parsed TOML top-level table.
 * @param err            Buffer for a diagnostic message on failure.
 * @param err_len        Size of `err` in bytes.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type, invalid name or value,
 *         or allocation failure.
 */
static int copy_derivatives(struct GalleryConfig* gallery_config,
                            toml_datum_t table,
                            char* err,
                            size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Reports whether a derivative name is a bounded safe identifier.
 *
 * @param name     Name bytes to check, not necessarily terminated. Must not be `NULL`.
 * @param name_len Length of `name` in bytes.
 * @return `true` when `name` matches `[A-Za-z][A-Za-z0-9_]*` within
 *         `GALLERY_DERIVATIVE_NAME_LEN_MAX` bytes, `false` otherwise.
 */
static bool is_derivative_name(const char* name, size_t name_len) __attribute__((nonnull(1)));

/**
 * @brief Validates and copies one bounded non-negative integer key into `spec->field`.
 *
 * @param table      Parsed TOML table holding `spec->key`.
 * @param key_prefix Dotted path of `table` that diagnostics put before `spec->key`, such as
 *                   `derivatives.tiny.`, or `""` for the top-level table. Must not be `NULL`.
 * @param spec       Key policy and target field. Must not be `NULL`.
 * @param err        Buffer for a diagnostic message on failure.
 * @param err_len    Size of `err` in bytes.
 * @return `0` on success or when an optional key is absent, or `-1` on a missing required key, a
 *         wrong type, a value below `spec->value_min`, or a value exceeding `spec->value_max`.
 */
static int copy_size(toml_datum_t table,
                     const char* key_prefix,
                     const struct GalleryConfigIntKey* spec,
                     char* err,
                     size_t err_len) __attribute__((nonnull(2, 3)));

/**
 * @brief Validates a present `base_url` as an absolute HTTP(S) URL and trims any trailing `/`.
 *
 * `copy_string` accepts a relative or scheme-less value, because it checks only presence and type.
 * Such a value then silently produces broken links in every absolute URL a template builds from it.
 * Checking it here keeps that a single config error. An empty `base_url` means the key is absent
 * and passes unchanged.
 *
 * @param gallery_config Config whose `base_url` is validated and normalized. Must not be `NULL`.
 * @param err            Buffer for a diagnostic message on failure.
 * @param err_len        Size of `err` in bytes.
 * @return `0` on success, or `-1` when the URL is not absolute or the copy fails.
 */
static int normalize_base_url(struct GalleryConfig* gallery_config, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether a URL is absolute: an HTTP(S) scheme followed by a non-empty host.
 *
 * @param url URL to check. Must not be `NULL`.
 * @return `true` when `url` has a recognized scheme and a host, `false` otherwise.
 */
static bool is_valid_base_url(const char* url) __attribute__((nonnull(1)));

/**
 * @brief Returns the bytes of `url` after `scheme`, matching ASCII letters case-insensitively.
 *
 * @param url    URL to inspect. Must not be `NULL`.
 * @param scheme Terminated scheme prefix to match, such as `https://`. Must not be `NULL`.
 * @return Pointer just past the scheme, or `NULL` when `url` does not start with it.
 */
static const char* skip_scheme(const char* url, const char* scheme) __attribute__((nonnull(1, 2)));

/**
 * @brief Trims trailing `/` from every configured directory and rejects an empty result.
 *
 * A trailing slash would otherwise survive into path construction, where it defeats the
 * `<input_dir>/` prefix strip that places each media item in its album. Normalizing once here keeps
 * that rule in one place and makes `gallery_config_print` round-trip the normalized form. The
 * location is not otherwise constrained. An absolute or parent-relative directory stays valid.
 *
 * @param gallery_config Config whose `input_dir`, `output_dir`, `templates_dir`, and `assets_dir`
 *                       are normalized. Must not be `NULL`.
 * @param err            Buffer for a diagnostic message on failure.
 * @param err_len        Size of `err` in bytes.
 * @return `0` on success, or `-1` when a directory is empty or trims to empty.
 */
static int normalize_dirs(struct GalleryConfig* gallery_config, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Trims trailing `/` from one configured directory in place, rejecting an empty result.
 *
 * This has a plain readable name rather than a `normalize_dirs_` prefix, because it is the whole
 * operation applied to one directory rather than one step of a decomposition. The caller is four
 * calls to it.
 *
 * @param value   Address of the config field holding the directory. Rewritten only when a trailing
 *                `/` was trimmed, so an already-normalized value costs no allocation. Must not be
 *                `NULL`.
 * @param key     Config key name used in diagnostics. Must not be `NULL`.
 * @param arena   Arena that owns the trimmed copy. Must not be `NULL`.
 * @param err     Buffer for a diagnostic message on failure.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` when the directory is empty, trims to empty, or cannot be copied.
 */
static int normalize_dir(const char** value,
                         const char* key,
                         struct Arena* arena,
                         char* err,
                         size_t err_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Returns the length of `value` with any trailing `/` bytes excluded.
 *
 * @param value Terminated value to measure. Must not be `NULL`.
 * @return Length in bytes up to the first byte of the trailing `/` run, or `0` when `value` is
 *         entirely `/`.
 */
static size_t trimmed_slash_len(const char* value) __attribute__((nonnull(1)));

/**
 * @brief Rejects a derivative whose bounding box exceeds `IMAGE_PIXEL_COUNT_MAX`.
 *
 * @param gallery_config Config whose derivatives are validated. Must not be `NULL`.
 * @param err            Buffer for a diagnostic message on failure.
 * @param err_len        Size of `err` in bytes.
 * @return `0` when every derivative fits, or `-1` on the first that does not.
 */
static int require_derivative_bounds(const struct GalleryConfig* gallery_config,
                                     char* err,
                                     size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Writes one `key = "value"` line, escaping the value as a TOML basic string.
 *
 * Delegates to `print_string` so a scalar value quotes exactly like array items.
 *
 * @param stream Destination stream. Must not be `NULL`.
 * @param key    Key name to write. Must not be `NULL`.
 * @param value  Value to quote and write. Must not be `NULL`.
 */
static void gallery_config_print_key_string(FILE* stream, const char* key, const char* value)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Writes one `key = [...]` line of TOML basic strings.
 *
 * Delegates to `print_string` so array items quote exactly like scalar values.
 *
 * @param stream     Destination stream. Must not be `NULL`.
 * @param key        Key name to write. Must not be `NULL`.
 * @param items      Array of `item_count` terminated strings. Must not be `NULL`.
 * @param item_count Number of items in `items`.
 */
static void gallery_config_print_string_array(FILE* stream,
                                              const char* key,
                                              const char* const* items,
                                              size_t item_count) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Writes `text` as a quoted TOML basic string, escaping delimiters and control bytes.
 *
 * TOML basic strings forbid raw control bytes, so this escapes them alongside `"` and `\`.
 *
 * @param stream Destination stream. Must not be `NULL`.
 * @param text   Terminated text to quote and escape. Must not be `NULL`.
 */
static void print_string(FILE* stream, const char* text) __attribute__((nonnull(1, 2)));

void gallery_config_init(struct GalleryConfig* gallery_config) {
  // This is one literal naming only the non-zero defaults, so a key added to `GalleryConfig` cannot
  // be left out here. A zeroed arena is a valid initialized arena, which is what `arena_init`
  // writes. The two required keys stay `NULL` until the load fills them.
  *gallery_config = (struct GalleryConfig){
      .base_url = "",
      .input_dir = INPUT_DIR_DEFAULT,
      .output_dir = OUTPUT_DIR_DEFAULT,
      .templates_dir = TEMPLATES_DIR_DEFAULT,
      .assets_dir = ASSETS_DIR_DEFAULT,
      .album_template = ALBUM_TEMPLATE_DEFAULT,
      .aggregate_templates = AGGREGATE_TEMPLATES_DEFAULT,
      .aggregate_template_count = 0,
      .derivatives = DERIVATIVES_DEFAULT,
      .derivative_count = sizeof(DERIVATIVES_DEFAULT) / sizeof(DERIVATIVES_DEFAULT[0]),
      .video_frame_seconds = VIDEO_FRAME_SECONDS_DEFAULT,
  };
}

void gallery_config_free(struct GalleryConfig* gallery_config) {
  arena_free(&gallery_config->arena);
  gallery_config_init(gallery_config);
}

int gallery_config_load(struct GalleryConfig* gallery_config,
                        const char* config_path,
                        char* err,
                        size_t err_len) {
  // This is zero-initialized, although every path reaching `toml_free` writes it. The struct passes
  // to `toml_free` by value, and without this clang-analyzer cannot prove across the call boundary
  // that its fields are set.
  toml_result_t parsed = {0};
  if (gallery_config_load_toml(config_path, &parsed, err, err_len) != 0) {
    return -1;
  }

  const int rc = gallery_config_load_fields(gallery_config, parsed.toptab, err, err_len);
  toml_free(parsed);
  return rc;
}

int gallery_config_print(FILE* stream, const struct GalleryConfig* gallery_config) {
  gallery_config_print_key_string(stream, "title", gallery_config->title);
  gallery_config_print_key_string(stream, "author", gallery_config->author);
  gallery_config_print_key_string(stream, "base_url", gallery_config->base_url);
  gallery_config_print_key_string(stream, "input_dir", gallery_config->input_dir);
  gallery_config_print_key_string(stream, "output_dir", gallery_config->output_dir);
  gallery_config_print_key_string(stream, "templates_dir", gallery_config->templates_dir);
  gallery_config_print_key_string(stream, "assets_dir", gallery_config->assets_dir);
  gallery_config_print_key_string(stream, "album_template", gallery_config->album_template);
  gallery_config_print_string_array(stream, "aggregate_templates",
                                    gallery_config->aggregate_templates,
                                    gallery_config->aggregate_template_count);
  fputs("derivatives = { ", stream);
  for (size_t i = 0; i < gallery_config->derivative_count; i++) {
    const struct GalleryDerivative* derivative = &gallery_config->derivatives[i];
    if (i > 0) {
      fputs(", ", stream);
    }
    fprintf(stream, "%s = { width = %zu, height = %zu, quality = %zu", derivative->name,
            derivative->width_px, derivative->height_px, derivative->quality);
    if (derivative->is_crop) {
      fputs(", crop = true", stream);
    }
    fputs(" }", stream);
  }
  fputs(" }\n", stream);
  fprintf(stream, "video_frame_seconds = %zu\n", gallery_config->video_frame_seconds);
  // A stream buffered to a pipe or file only surfaces a failed write at flush time, not at the
  // individual `fprintf`/`fputc` calls above.
  if (fflush(stream) != 0) {
    return -1;
  }
  if (ferror(stream) != 0) {
    // The stream latched an error from an earlier call, whose `errno` may have been overwritten
    // since. Name a generic I/O failure rather than let the caller relay a stale value.
    errno = EIO;
    return -1;
  }
  return 0;
}

char* gallery_config_derivative_recipe(const struct GalleryConfig* gallery_config,
                                       struct Arena* arena) {
  char recipe[RECIPE_COMPONENT_SIZE];
  const size_t frame_ms = gallery_config->video_frame_seconds * 1000;
  int recipe_len = snprintf(recipe, sizeof(recipe), "v%d", DERIVATIVE_RECIPE_VERSION);
  if (recipe_len < 0 || (size_t)recipe_len >= sizeof(recipe)) {
    return NULL;
  }
  size_t used = (size_t)recipe_len;
  for (size_t i = 0; i < gallery_config->derivative_count; i++) {
    const struct GalleryDerivative* derivative = &gallery_config->derivatives[i];
    recipe_len = snprintf(recipe + used, sizeof(recipe) - used, "-%s-%zux%zu%s-q%zu",
                          derivative->name, derivative->width_px, derivative->height_px,
                          derivative->is_crop ? "c" : "", derivative->quality);
    if (recipe_len < 0 || (size_t)recipe_len >= sizeof(recipe) - used) {
      return NULL;
    }
    used += (size_t)recipe_len;
  }
  recipe_len = snprintf(recipe + used, sizeof(recipe) - used, "-f%zu", frame_ms);
  if (recipe_len < 0 || (size_t)recipe_len >= sizeof(recipe) - used) {
    return NULL;
  }
  return arena_strdup(arena, recipe);
}

static int gallery_config_load_toml(const char* config_path,
                                    toml_result_t* parsed_out,
                                    char* err,
                                    size_t err_len) {
  unsigned char* config_data = NULL;
  size_t config_len = 0;
  char reason[FS_REASON_SIZE];
  if (fs_read_file(config_path, CONFIG_FILE_LEN_MAX, &config_data, &config_len, reason,
                   sizeof(reason)) != 0) {
    return error_report(err, err_len, "failed to read config: %s ('%s')", reason, config_path);
  }

  int rc = -1;
  if (!text_is_nul_free(config_data, config_len)) {
    (void)error_report(err, err_len, "failed to read config: contains an embedded NUL byte ('%s')",
                       config_path);
    goto cleanup;
  }
  // tomlc17 copies the input, so cleanup frees the source buffer once. The cast is exact because
  // the read bounds `config_len` at `CONFIG_FILE_LEN_MAX`, which fits an `int`.
  *parsed_out = toml_parse_named((const char*)config_data, (int)config_len, config_path);
  if (!parsed_out->ok) {
    (void)error_report(err, err_len, "failed to parse config: %s ('%s')", parsed_out->errmsg,
                       config_path);
    toml_free(*parsed_out);
    goto cleanup;
  }
  rc = 0;

cleanup:
  free(config_data);
  return rc;
}

static int gallery_config_load_fields(struct GalleryConfig* gallery_config,
                                      toml_datum_t table,
                                      char* err,
                                      size_t err_len) {
  if (toml_require_known_keys(table, config_keys, sizeof(config_keys) / sizeof(config_keys[0]),
                              "config", "", err, err_len) != 0) {
    return -1;
  }

  struct GalleryConfigStringKey strings[] = {
      {"title", &gallery_config->title, true},
      {"author", &gallery_config->author, true},
      {"base_url", &gallery_config->base_url, false},
      {"input_dir", &gallery_config->input_dir, false},
      {"output_dir", &gallery_config->output_dir, false},
      {"templates_dir", &gallery_config->templates_dir, false},
      {"assets_dir", &gallery_config->assets_dir, false},
      {"album_template", &gallery_config->album_template, false},
  };

  for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
    if (copy_string(table, strings[i].key, strings[i].is_required, &gallery_config->arena,
                    strings[i].field, err, err_len) != 0) {
      return -1;
    }
  }

  if (normalize_base_url(gallery_config, err, err_len) != 0) {
    return -1;
  }
  if (normalize_dirs(gallery_config, err, err_len) != 0) {
    return -1;
  }
  if (!path_is_safe_relative(gallery_config->album_template)) {
    return error_report(err, err_len,
                        "config key 'album_template' must be a safe relative template name: '%s'",
                        gallery_config->album_template);
  }
  if (copy_template_names(table, "aggregate_templates", &gallery_config->arena,
                          &gallery_config->aggregate_templates,
                          &gallery_config->aggregate_template_count, err, err_len) != 0) {
    return -1;
  }
  if (copy_derivatives(gallery_config, table, err, err_len) != 0) {
    return -1;
  }

  const struct GalleryConfigIntKey video_frame_seconds = {
      .key = "video_frame_seconds",
      .field = &gallery_config->video_frame_seconds,
      .value_max = VIDEO_FRAME_SECONDS_MAX,
      .limit_name = "video frame seconds",
  };
  if (copy_size(table, "", &video_frame_seconds, err, err_len) != 0) {
    return -1;
  }
  return require_derivative_bounds(gallery_config, err, err_len);
}

static int copy_string(toml_datum_t table,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       const char** value_out,
                       char* err,
                       size_t err_len) {
  const toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    if (is_required) {
      return error_report(err, err_len, "missing required config key '%s'", key);
    }
    return 0;
  }
  if (value.type != TOML_STRING) {
    return error_report(err, err_len, "config key '%s' must be a string", key);
  }
  if (!toml_datum_is_text(value)) {
    return error_report(err, err_len, "config key '%s' must not contain a NUL byte", key);
  }
  *value_out = arena_strndup(arena, value.u.str.ptr, (size_t)value.u.str.len);
  if (*value_out == NULL) {
    return error_report(err, err_len, "out of memory reading config key '%s'", key);
  }
  return 0;
}

static int copy_template_names(toml_datum_t table,
                               const char* key,
                               struct Arena* arena,
                               const char* const** template_names_out,
                               size_t* template_name_count_out,
                               char* err,
                               size_t err_len) {
  const toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_ARRAY) {
    return error_report(err, err_len, "config key '%s' must be an array", key);
  }

  const size_t item_count = (size_t)value.u.arr.size;
  // This allocates even for an empty array, rather than leaving it `NULL`. `arena_alloc` normalizes
  // a zero-byte request to a distinct one-byte allocation, so this never yields `NULL`. Every
  // consumer of the published array is declared `nonnull` on it. If left `NULL`,
  // `aggregate_templates = []` would reach `gallery_config_print_string_array` as a null pointer
  // and abort.
  const char** items = arena_calloc(arena, item_count, sizeof(*items));
  if (items == NULL) {
    return error_report(err, err_len, "out of memory reading config key '%s'", key);
  }

  for (size_t i = 0; i < item_count; i++) {
    if (value.u.arr.elem[i].type != TOML_STRING) {
      return error_report(err, err_len, "config key '%s' must contain only strings", key);
    }
    if (!toml_datum_is_text(value.u.arr.elem[i])) {
      return error_report(err, err_len, "config key '%s' must not contain a NUL byte", key);
    }
    items[i] =
        arena_strndup(arena, value.u.arr.elem[i].u.str.ptr, (size_t)value.u.arr.elem[i].u.str.len);
    if (items[i] == NULL) {
      return error_report(err, err_len, "out of memory reading config key '%s'", key);
    }
    if (!path_is_safe_relative(items[i])) {
      return error_report(err, err_len,
                          "config key '%s' must contain only safe relative template names: '%s'",
                          key, items[i]);
    }
  }

  // The code fills these through a mutable local and publishes them as `const char* const*`. The
  // elements have to be writable while it builds the arena copy, but must not be once the config
  // shares them with the render workers.
  *template_names_out = items;
  *template_name_count_out = item_count;
  return 0;
}

static int copy_derivatives(struct GalleryConfig* gallery_config,
                            toml_datum_t table,
                            char* err,
                            size_t err_len) {
  const toml_datum_t value = toml_get(table, "derivatives");
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_TABLE) {
    return error_report(err, err_len, "config key 'derivatives' must be a table");
  }
  if (value.u.tab.size < 1) {
    return error_report(err, err_len, "config key 'derivatives' must not be empty");
  }
  if ((uint32_t)value.u.tab.size > GALLERY_DERIVATIVE_COUNT_MAX) {
    return error_report(err, err_len,
                        "config key 'derivatives' exceeds max derivative count (%d) at %d",
                        GALLERY_DERIVATIVE_COUNT_MAX, value.u.tab.size);
  }

  const size_t derivative_count = (size_t)value.u.tab.size;
  struct GalleryDerivative* derivatives =
      arena_calloc(&gallery_config->arena, derivative_count, sizeof(*derivatives));
  if (derivatives == NULL) {
    return error_report(err, err_len, "out of memory reading config key 'derivatives'");
  }
  for (size_t i = 0; i < derivative_count; i++) {
    const char* source_name = value.u.tab.key[i];
    const size_t name_len = (size_t)value.u.tab.len[i];
    if (!is_derivative_name(source_name, name_len)) {
      return error_report(
          err, err_len,
          "config key 'derivatives' name must match [A-Za-z][A-Za-z0-9_]* and be at "
          "most %d bytes: '%.*s'",
          GALLERY_DERIVATIVE_NAME_LEN_MAX, value.u.tab.len[i], source_name);
    }
    derivatives[i].name = arena_strndup(&gallery_config->arena, source_name, name_len);
    if (derivatives[i].name == NULL) {
      return error_report(err, err_len, "out of memory reading config key 'derivatives'");
    }

    const toml_datum_t derivative_table = value.u.tab.value[i];
    if (derivative_table.type != TOML_TABLE) {
      return error_report(err, err_len, "config key 'derivatives.%s' must be a table",
                          derivatives[i].name);
    }
    char key_prefix[sizeof("derivatives..") + GALLERY_DERIVATIVE_NAME_LEN_MAX];
    (void)snprintf(key_prefix, sizeof(key_prefix), "derivatives.%s.", derivatives[i].name);
    if (toml_require_known_keys(derivative_table, derivative_keys,
                                sizeof(derivative_keys) / sizeof(derivative_keys[0]), "config",
                                key_prefix, err, err_len) != 0) {
      return -1;
    }
    const struct GalleryConfigIntKey integers[] = {
        {"width", &derivatives[i].width_px, 1, IMAGE_DIMENSION_MAX, "image dimension", true},
        {"height", &derivatives[i].height_px, 1, IMAGE_DIMENSION_MAX, "image dimension", true},
        {"quality", &derivatives[i].quality, 1, 100, "JPEG quality", true},
    };
    for (size_t j = 0; j < sizeof(integers) / sizeof(integers[0]); j++) {
      if (copy_size(derivative_table, key_prefix, &integers[j], err, err_len) != 0) {
        return -1;
      }
    }

    const toml_datum_t crop = toml_get(derivative_table, "crop");
    if (crop.type != TOML_UNKNOWN && crop.type != TOML_BOOLEAN) {
      return error_report(err, err_len, "config key 'derivatives.%s.crop' must be a boolean",
                          derivatives[i].name);
    }
    derivatives[i].is_crop = crop.type == TOML_BOOLEAN && crop.u.boolean;
  }
  gallery_config->derivatives = derivatives;
  gallery_config->derivative_count = derivative_count;
  return 0;
}

static bool is_derivative_name(const char* name, size_t name_len) {
  if (name_len == 0 || name_len > GALLERY_DERIVATIVE_NAME_LEN_MAX ||
      !((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'))) {
    return false;
  }
  for (size_t i = 1; i < name_len; i++) {
    if (!ascii_is_alphanumeric((unsigned char)name[i]) && name[i] != '_') {
      return false;
    }
  }
  return true;
}

static int copy_size(toml_datum_t table,
                     const char* key_prefix,
                     const struct GalleryConfigIntKey* spec,
                     char* err,
                     size_t err_len) {
  const toml_datum_t value = toml_get(table, spec->key);
  if (value.type == TOML_UNKNOWN) {
    if (spec->is_required) {
      return error_report(err, err_len, "missing required config key '%s%s'", key_prefix,
                          spec->key);
    }
    return 0;
  }
  if (value.type != TOML_INT64) {
    return error_report(err, err_len, "config key '%s%s' must be an integer", key_prefix,
                        spec->key);
  }
  if (value.u.int64 < 0 || (uint64_t)value.u.int64 < (uint64_t)spec->value_min) {
    return error_report(err, err_len, "config key '%s%s' must be at least %zu at %jd", key_prefix,
                        spec->key, spec->value_min, (intmax_t)value.u.int64);
  }
  // One comparison covers both the configured ceiling and the cast below. `value_max` is a
  // `size_t`, so a value this check accepts fits `size_t` on every target.
  if ((uint64_t)value.u.int64 > (uint64_t)spec->value_max) {
    return error_report(err, err_len, "config key '%s%s' exceeds max %s (%zu) at %ju", key_prefix,
                        spec->key, spec->limit_name, spec->value_max, (uintmax_t)value.u.int64);
  }
  *spec->field = (size_t)value.u.int64;
  return 0;
}

static int normalize_base_url(struct GalleryConfig* gallery_config, char* err, size_t err_len) {
  if (gallery_config->base_url[0] == '\0') {
    return 0;
  }
  if (!is_valid_base_url(gallery_config->base_url)) {
    return error_report(err, err_len,
                        "config key 'base_url' must be an absolute 'http://' or 'https://' URL "
                        "with a host: '%s'",
                        gallery_config->base_url);
  }

  // Trim a trailing `/` so templates can join `base_url` with a path that starts with one without
  // producing `//`. Normalizing once here keeps the rule in one place and makes
  // `gallery_config_print` round-trip the normalized form, as `normalize_dirs` does for
  // directories. The trim cannot eat the scheme's slashes, because the check above proved a host
  // byte follows them.
  const size_t value_len = strlen(gallery_config->base_url);
  const size_t trimmed_len = trimmed_slash_len(gallery_config->base_url);
  if (trimmed_len < value_len) {
    gallery_config->base_url =
        arena_strndup(&gallery_config->arena, gallery_config->base_url, trimmed_len);
    if (gallery_config->base_url == NULL) {
      return error_report(err, err_len, "out of memory reading config key 'base_url'");
    }
  }
  return 0;
}

static bool is_valid_base_url(const char* url) {
  /**
   * It accepts a scheme plus a non-empty host, and nothing more. A port, subpath, or query string
   * must stay valid, and judging a host beyond "not empty" would mean ruling on what DNS and
   * punycode allow. Schemes are case-insensitive, so `HTTPS://` is the same URL.
   */
  static const char* const URL_SCHEMES[] = {"http://", "https://"};

  for (size_t i = 0; i < sizeof(URL_SCHEMES) / sizeof(URL_SCHEMES[0]); i++) {
    const char* host = skip_scheme(url, URL_SCHEMES[i]);
    if (host != NULL) {
      // `https://` has no host at all. `https:///path`, `https://?q` and `https://#f` have an empty
      // host: `/`, `?`, or `#` starts immediately.
      return *host != '\0' && *host != '/' && *host != '?' && *host != '#';
    }
  }
  return false;
}

static const char* skip_scheme(const char* url, const char* scheme) {
  size_t i = 0;
  // A `url` shorter than `scheme` stops at its terminator, which no scheme byte matches.
  for (; scheme[i] != '\0'; i++) {
    if (ascii_to_lower((unsigned char)url[i]) != ascii_to_lower((unsigned char)scheme[i])) {
      return NULL;
    }
  }
  return url + i;
}

static int normalize_dirs(struct GalleryConfig* gallery_config, char* err, size_t err_len) {
  if (normalize_dir(&gallery_config->input_dir, "input_dir", &gallery_config->arena, err,
                    err_len) != 0) {
    return -1;
  }
  if (normalize_dir(&gallery_config->output_dir, "output_dir", &gallery_config->arena, err,
                    err_len) != 0) {
    return -1;
  }
  if (normalize_dir(&gallery_config->templates_dir, "templates_dir", &gallery_config->arena, err,
                    err_len) != 0) {
    return -1;
  }
  return normalize_dir(&gallery_config->assets_dir, "assets_dir", &gallery_config->arena, err,
                       err_len);
}

static int normalize_dir(const char** value,
                         const char* key,
                         struct Arena* arena,
                         char* err,
                         size_t err_len) {
  const size_t value_len = strlen(*value);
  const size_t trimmed_len = trimmed_slash_len(*value);
  if (trimmed_len == 0) {
    return error_report(err, err_len, "config key '%s' must not be empty", key);
  }
  if (trimmed_len < value_len) {
    *value = arena_strndup(arena, *value, trimmed_len);
    if (*value == NULL) {
      return error_report(err, err_len, "out of memory reading config key '%s'", key);
    }
  }
  return 0;
}

static size_t trimmed_slash_len(const char* value) {
  size_t len = strlen(value);
  while (len > 0 && value[len - 1] == '/') {
    len--;
  }
  return len;
}

static int require_derivative_bounds(const struct GalleryConfig* gallery_config,
                                     char* err,
                                     size_t err_len) {
  for (size_t i = 0; i < gallery_config->derivative_count; i++) {
    const struct GalleryDerivative* derivative = &gallery_config->derivatives[i];
    if (derivative->height_px > IMAGE_PIXEL_COUNT_MAX / derivative->width_px) {
      return error_report(err, err_len,
                          "config key 'derivatives.%s' dimensions exceed max image pixel count "
                          "(%zu) at %zux%zu",
                          derivative->name, (size_t)IMAGE_PIXEL_COUNT_MAX, derivative->width_px,
                          derivative->height_px);
    }
  }
  return 0;
}

static void gallery_config_print_key_string(FILE* stream, const char* key, const char* value) {
  fprintf(stream, "%s = ", key);
  print_string(stream, value);
  fputc('\n', stream);
}

static void gallery_config_print_string_array(FILE* stream,
                                              const char* key,
                                              const char* const* items,
                                              size_t item_count) {
  fprintf(stream, "%s = [", key);
  for (size_t i = 0; i < item_count; i++) {
    if (i > 0) {
      fputs(", ", stream);
    }
    print_string(stream, items[i]);
  }
  fputs("]\n", stream);
}

static void print_string(FILE* stream, const char* text) {
  fputc('"', stream);
  // Walk the bytes as `unsigned char`. Where `char` is signed, every byte of a multi-byte UTF-8
  // sequence is negative, so the `< 0x20` test below would treat it as a control byte, and passing
  // a negative `int` to the `%04X` conversion is undefined.
  for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; p++) {
    switch (*p) {
      case '"':
        fputs("\\\"", stream);
        break;
      case '\\':
        fputs("\\\\", stream);
        break;
      case '\b':
        fputs("\\b", stream);
        break;
      case '\t':
        fputs("\\t", stream);
        break;
      case '\n':
        fputs("\\n", stream);
        break;
      case '\f':
        fputs("\\f", stream);
        break;
      case '\r':
        fputs("\\r", stream);
        break;
      default:
        if (*p < 0x20 || *p == 0x7F) {
          fprintf(stream, "\\u%04X", *p);
        } else {
          fputc(*p, stream);
        }
        break;
    }
  }
  fputc('"', stream);
}
