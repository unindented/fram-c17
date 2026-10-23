#include "domain/gallery_config.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <tomlc17.h>

#include "core/ascii.h"
#include "core/path.h"
#include "core/text.h"
#include "formats/image.h"
#include "formats/toml.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** String-valued gallery config key policy. */
struct GalleryConfigStringKey {
  /** TOML key name. */
  const char* key;

  /** Address of the `GalleryConfig` field this key fills. */
  struct Str* field;

  /** Whether absence is a load error rather than leaving the initialized default. */
  bool is_required;
};

/** Integer-valued gallery config key policy. */
struct GalleryConfigIntKey {
  /** TOML key name. */
  const char* key;

  /** Address of the `GalleryConfig` field this key fills. */
  ptrdiff_t* field;

  /** Smallest accepted value, named in the `must be at least` rejection. */
  ptrdiff_t value_min;

  /** Largest accepted value. A larger one is rejected as exceeding `limit_name`. */
  ptrdiff_t value_max;

  /** Prose name of `value_max` in the overflow diagnostic: `exceeds max video frame seconds`. */
  const char* limit_name;

  /** Whether absence is a load error rather than leaving the field's current value. */
  bool is_required;
};

const struct Str GALLERY_CONFIG_PATH_DEFAULT = STR_INIT("fram.toml");

#define INPUT_DIR_DEFAULT S("media")
#define OUTPUT_DIR_DEFAULT S("public")
#define TEMPLATES_DIR_DEFAULT S("templates")
#define STATIC_DIR_DEFAULT S("static")
#define ALBUM_TEMPLATE_DEFAULT S("album.html")

// The root album already renders to `index.html`. A default aggregate of that name would make the
// default configuration fail its manifest phase, so the default array is intentionally empty.
static const struct Str AGGREGATE_TEMPLATES_DEFAULT[] = {{0}};

/**
 * Every key the top-level table may hold. The load rejects a key outside this list, so a misspelled
 * key cannot silently leave its default in place. A key `gallery_config_load_fields` reads must be
 * listed here.
 */
static const struct Str config_keys[] = {
    // clang-format off: one key per line, so adding or removing a key is a one-line change.
    STR_INIT("title"),
    STR_INIT("author"),
    STR_INIT("base_url"),
    STR_INIT("input_dir"),
    STR_INIT("output_dir"),
    STR_INIT("templates_dir"),
    STR_INIT("static_dir"),
    STR_INIT("album_template"),
    STR_INIT("aggregate_templates"),
    STR_INIT("derivatives"),
    STR_INIT("sort"),
    STR_INIT("video_frame_seconds"),
    // clang-format on
};

/** Every key a `derivatives.<name>` table may hold, each read by `copy_derivatives_entry`. */
static const struct Str derivative_keys[] = {STR_INIT("width"), STR_INIT("height"),
                                             STR_INIT("quality"), STR_INIT("crop")};

/** Every key the `sort` table may hold, each read by `copy_sort`. */
static const struct Str sort_keys[] = {STR_INIT("albums"), STR_INIT("media")};

static const struct GalleryDerivative DERIVATIVES_DEFAULT[] = {
    {.name = STR_INIT("s"), .width_px = 120, .height_px = 120, .quality = 70, .is_crop = true},
    {.name = STR_INIT("m"), .width_px = 520, .height_px = 360, .quality = 80, .is_crop = true},
    {.name = STR_INIT("l"), .width_px = 792, .height_px = 594, .quality = 85, .is_crop = false},
};

enum {
  /** Video-frame timestamp used when `video_frame_seconds` is absent from the configuration. */
  VIDEO_FRAME_SECONDS_DEFAULT = 1,

  /** Largest accepted `video_frame_seconds`, one day in seconds. */
  VIDEO_FRAME_SECONDS_MAX = 86400,
};

/**
 * Largest config file read, in bytes.
 *
 * A config is a few dozen keys and derivative tables, well under a kilobyte, so 1 MiB is three
 * orders of magnitude of headroom while keeping what tomlc17 copies and builds from it to a few
 * MiB. The read checks it before loading the file, so an oversize config is never resident.
 */
enum { CONFIG_FILE_LEN_MAX = 1024 * 1024 };

// `toml_parse_document` takes only a document whose length fits an `int`.
_Static_assert(CONFIG_FILE_LEN_MAX <= INT_MAX,
               "an accepted config must fit the int length tomlc17 takes");

/**
 * @brief Reads and parses a TOML config file into a `toml_result_t`.
 *
 * On success `parsed_out` owns the parse result until the caller runs `toml_free`.
 *
 * @param config_path Path to the config file.
 * @param os          Host the file is read from. Must not be `NULL`.
 * @param scratch     Arena for the file bytes, which tomlc17 copies.
 * @param parsed_out  Receives the parsed TOML result on success. Must not be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` on a read, size, embedded `NUL`, or TOML parse error.
 */
static int gallery_config_load_toml(struct Str config_path,
                                    struct Os* os,
                                    struct Arena scratch,
                                    toml_result_t* parsed_out,
                                    struct Log* log) __attribute__((nonnull(2, 4, 5)));

/**
 * @brief Copies recognized configuration keys into gallery config storage.
 *
 * Rejects a key outside `config_keys`, then applies required and optional string keys, the
 * template-name array, the derivative tables, and `video_frame_seconds`. It copies every value into
 * `perm` rather than borrowing from `table`. `gallery_config_load` calls `toml_free` as soon as
 * this returns, which releases the pool every `toml_datum_t` string points into, while the config
 * outlives the parse. The `sort` table is applied just before `video_frame_seconds`.
 *
 * @param gallery_config Config that receives the values. Must not be `NULL`.
 * @param table          Parsed TOML top-level table.
 * @param perm           Arena that owns the copied values. Must not be `NULL`.
 * @param log            Log that receives every error. Must not be `NULL`.
 */
static void gallery_config_load_fields(struct GalleryConfig* gallery_config,
                                       toml_datum_t table,
                                       struct Arena* perm,
                                       struct Log* log) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Copies one string key into the arena, enforcing required keys.
 *
 * @param table       Parsed TOML table.
 * @param key         Key to read. Must not be `NULL`.
 * @param is_required Whether absence is an error. When `false`, absence leaves `*value_out`
 *                    untouched.
 * @param arena       Arena that owns the copied value. Must not be `NULL`.
 * @param value_out   Receives the copied string when the key is present. Must not be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` if a required key is missing or mistyped.
 */
static int copy_string(toml_datum_t table,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       struct Str* value_out,
                       struct Log* log) __attribute__((nonnull(2, 4, 5, 6)));

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
 * @param log                     Log that receives every error. Must not be `NULL`.
 * @return `0` on success or when absent, or `-1` on a wrong type, a `NUL` byte, or an unsafe name.
 */
static int copy_template_names(toml_datum_t table,
                               const char* key,
                               struct Arena* arena,
                               const struct Str** template_names_out,
                               ptrdiff_t* template_name_count_out,
                               struct Log* log) __attribute__((nonnull(2, 3, 4, 5, 6)));

/**
 * @brief Copies and validates the optional named-derivative table.
 *
 * When the key is absent, this leaves `derivatives` untouched and keeps the defaults from
 * `gallery_config_init`. It checks every derivative even after one fails, and publishes them only
 * when every one is valid, so a rejected table also keeps the defaults.
 *
 * @param gallery_config Config that receives the derivatives. Must not be `NULL`.
 * @param table          Parsed TOML top-level table.
 * @param perm           Arena that owns the copied derivatives. Must not be `NULL`.
 * @param log            Log that receives every error. Must not be `NULL`.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type or an invalid name or
 *         value.
 */
static int copy_derivatives(struct GalleryConfig* gallery_config,
                            toml_datum_t table,
                            struct Arena* perm,
                            struct Log* log) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Copies and validates one `derivatives.<name>` entry.
 *
 * It checks every key of the entry even after one fails, so one run reports each mistake in it.
 *
 * @param name           Entry name, the key in the `derivatives` table.
 * @param entry_table    Entry value, which must be a table.
 * @param perm           Arena that owns the copied name. Must not be `NULL`.
 * @param derivative_out Receives the name, dimensions, quality and crop flag that were accepted.
 *                       Must not be `NULL`.
 * @param log            Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` on an invalid name, a wrong type, an unknown key, or an invalid
 *         value.
 */
static int copy_derivatives_entry(struct Str name,
                                  toml_datum_t entry_table,
                                  struct Arena* perm,
                                  struct GalleryDerivative* derivative_out,
                                  struct Log* log) __attribute__((nonnull(3, 4, 5)));

/**
 * @brief Reports whether a derivative name is a bounded safe identifier.
 *
 * @param name Name to check.
 * @return `true` when `name` matches `[A-Za-z][A-Za-z0-9_]*` within
 *         `GALLERY_DERIVATIVE_NAME_LEN_MAX` bytes, `false` otherwise.
 */
static bool is_derivative_name(struct Str name);

/**
 * @brief Validates and copies one bounded non-negative integer key into `spec->field`.
 *
 * @param table      Parsed TOML table holding `spec->key`.
 * @param key_prefix Dotted path of `table` that diagnostics put before `spec->key`, such as
 *                   `derivatives.tiny.`, or `""` for the top-level table.
 * @param spec       Key policy and target field. Must not be `NULL`.
 * @param log        Log that receives every error. Must not be `NULL`.
 * @return `0` on success or when an optional key is absent, or `-1` on a missing required key, a
 *         wrong type, a value below `spec->value_min`, or a value exceeding `spec->value_max`.
 */
static int copy_size(toml_datum_t table,
                     struct Str key_prefix,
                     const struct GalleryConfigIntKey* spec,
                     struct Log* log) __attribute__((nonnull(3, 4)));

/**
 * @brief Copies and validates the optional `sort` table.
 *
 * When the key is absent, this leaves both orders untouched and keeps the defaults from
 * `gallery_config_init`. An empty table also keeps them. It checks both keys even after one fails,
 * and a rejected key keeps its default.
 *
 * @param gallery_config Config that receives the orders. Must not be `NULL`.
 * @param table          Parsed TOML top-level table.
 * @param log            Log that receives every error. Must not be `NULL`.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type, an unknown key, or an
 *         invalid order.
 */
static int copy_sort(struct GalleryConfig* gallery_config, toml_datum_t table, struct Log* log)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Copies one optional `sort.<key>` order, which must be exactly `asc` or `desc`.
 *
 * @param sort_table Parsed `sort` table.
 * @param key        Key to read. Must not be `NULL`.
 * @param order_out  Receives the order when the key is present and valid, and keeps its value
 *                   otherwise. Must not be `NULL`.
 * @param log        Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type, a `NUL` byte, or a
 *         value other than `asc` or `desc`.
 */
static int copy_sort_order(toml_datum_t sort_table,
                           const char* key,
                           enum GallerySortOrder* order_out,
                           struct Log* log) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Validates a present `base_url` as an absolute HTTP(S) URL and trims any trailing `/`.
 *
 * `copy_string` accepts a relative or scheme-less value, because it checks only presence and type.
 * Such a value then silently produces broken links in every absolute URL a template builds from it.
 * Checking it here keeps that a single config error. An empty `base_url` means the key is absent
 * and passes unchanged. A rejected URL is cleared to that default.
 *
 * @param gallery_config Config whose `base_url` is validated and normalized. Must not be `NULL`.
 * @param log            Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` when the URL is not absolute.
 */
static int normalize_base_url(struct GalleryConfig* gallery_config, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether a URL is absolute: an HTTP(S) scheme followed by a non-empty host.
 *
 * @param url URL to check.
 * @return `true` when `url` has a recognized scheme and a host, `false` otherwise.
 */
static bool is_valid_base_url(struct Str url);

/**
 * @brief Reports whether `url` starts with `scheme`, matching ASCII letters case-insensitively.
 *
 * @param url    URL to inspect.
 * @param scheme Scheme prefix to match, such as `https://`.
 * @return `true` when `url` starts with `scheme`, `false` otherwise.
 */
static bool has_scheme(struct Str url, struct Str scheme);

/**
 * @brief Copies one optional directory key, trims its trailing `/` and rejects an empty result.
 *
 * A trailing slash would otherwise survive into path construction, where it defeats the
 * `<input_dir>/` prefix strip that places each media item in its album. Normalizing once here keeps
 * that rule in one place and makes `gallery_config_print` round-trip the normalized form. The
 * location is not otherwise constrained. An absolute or parent-relative directory stays valid.
 *
 * @param table     Parsed TOML top-level table.
 * @param key       Config key name, used in diagnostics. Must not be `NULL`.
 * @param fallback  Default the field holds after any rejection, so later phases still have a
 *                  directory.
 * @param arena     Arena that owns the copied value. Must not be `NULL`.
 * @param value_out Config field holding the directory, which keeps its value when the key is
 *                  absent. Must not be `NULL`.
 * @param log       Log that receives the error. Must not be `NULL`.
 * @return `0` when the key is absent or accepted, or `-1` when it is not a string, holds a `NUL`
 *         byte, or is empty once trimmed.
 */
static int copy_dir(toml_datum_t table,
                    const char* key,
                    struct Str fallback,
                    struct Arena* arena,
                    struct Str* value_out,
                    struct Log* log) __attribute__((nonnull(2, 4, 5, 6)));

/**
 * @brief Returns `value` with any trailing `/` bytes excluded.
 *
 * @param value Value to trim.
 * @return View of `value` up to the first byte of its trailing `/` run, empty when `value` is
 *         entirely `/`.
 */
static struct Str trim_trailing_slashes(struct Str value);

/**
 * @brief Rejects an `album_template` that is not a safe relative template name.
 *
 * A rejected name is reset to the default, so the render still has a template to look for.
 *
 * @param gallery_config Config whose `album_template` is validated. Must not be `NULL`.
 * @param log            Log that receives the error. Must not be `NULL`.
 * @return `0` when the name is safe, or `-1` when it is not.
 */
static int normalize_album_template(struct GalleryConfig* gallery_config, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Rejects every derivative whose bounding box exceeds `IMAGE_PIXEL_COUNT_MAX`.
 *
 * @param derivatives      Array of `derivative_count` derivatives to validate. Must not be `NULL`.
 * @param derivative_count Number of derivatives in `derivatives`.
 * @param log              Log that receives every error. Must not be `NULL`.
 * @return `0` when every derivative fits, or `-1` when any does not.
 */
static int require_derivative_bounds(const struct GalleryDerivative* derivatives,
                                     ptrdiff_t derivative_count,
                                     struct Log* log) __attribute__((nonnull(1, 3)));

/**
 * @brief Writes one `key = "value"` line, escaping the value as a TOML basic string.
 *
 * Delegates to `print_string` so a scalar value quotes exactly like array items.
 *
 * @param buffer Buffer the line is appended to. Must not be `NULL`.
 * @param key    Terminated key name to write. Must not be `NULL`.
 * @param value  Value to quote and write.
 */
static void gallery_config_print_key_string(struct Buf* buffer, const char* key, struct Str value)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes one `key = [...]` line of TOML basic strings.
 *
 * Delegates to `print_string` so array items quote exactly like scalar values.
 *
 * @param buffer     Buffer the line is appended to. Must not be `NULL`.
 * @param key        Terminated key name to write. Must not be `NULL`.
 * @param items      Array of `item_count` strings. Must not be `NULL`.
 * @param item_count Number of items in `items`.
 */
static void gallery_config_print_string_array(struct Buf* buffer,
                                              const char* key,
                                              const struct Str* items,
                                              ptrdiff_t item_count)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Writes the `sort = { ... }` line, naming both orders even at their defaults.
 *
 * The table is inline, so a key printed after it still belongs to the top-level table.
 *
 * @param buffer         Buffer the line is appended to. Must not be `NULL`.
 * @param gallery_config Config whose orders are written. Must not be `NULL`.
 */
static void gallery_config_print_sort(struct Buf* buffer,
                                      const struct GalleryConfig* gallery_config)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes `text` as a quoted TOML basic string, escaping delimiters and control bytes.
 *
 * TOML basic strings forbid raw control bytes, so this escapes them alongside `"` and `\`.
 *
 * @param buffer Buffer the quoted string is appended to. Must not be `NULL`.
 * @param text   Text to quote and escape.
 */
static void print_string(struct Buf* buffer, struct Str text) __attribute__((nonnull(1)));

void gallery_config_init(struct GalleryConfig* gallery_config) {
  // This is one literal naming only the non-zero defaults, so a key added to `GalleryConfig` cannot
  // be left out here. The two required keys stay empty until the load fills them.
  *gallery_config = (struct GalleryConfig){
      .input_dir = INPUT_DIR_DEFAULT,
      .output_dir = OUTPUT_DIR_DEFAULT,
      .templates_dir = TEMPLATES_DIR_DEFAULT,
      .static_dir = STATIC_DIR_DEFAULT,
      .album_template = ALBUM_TEMPLATE_DEFAULT,
      .aggregate_templates = AGGREGATE_TEMPLATES_DEFAULT,
      .aggregate_template_count = 0,
      .derivatives = DERIVATIVES_DEFAULT,
      .derivative_count = sizeof(DERIVATIVES_DEFAULT) / sizeof(DERIVATIVES_DEFAULT[0]),
      .video_frame_seconds = VIDEO_FRAME_SECONDS_DEFAULT,
  };
}

int gallery_config_load(struct GalleryConfig* gallery_config,
                        struct Str config_path,
                        struct Os* os,
                        struct Arena* perm,
                        struct Arena scratch,
                        struct Log* log) {
  // This is zero-initialized, although every path reaching `toml_free` writes it. The struct passes
  // to `toml_free` by value, and without this clang-analyzer cannot prove across the call boundary
  // that its fields are set.
  toml_result_t parsed = {0};
  if (gallery_config_load_toml(config_path, os, scratch, &parsed, log) != 0) {
    return -1;
  }

  // A key that fails validation is in `log` already, and the load still succeeded in the sense this
  // result reports: the file was read and parsed.
  gallery_config_load_fields(gallery_config, parsed.toptab, perm, log);
  toml_free(parsed);
  return 0;
}

int gallery_config_print(const struct GalleryConfig* gallery_config,
                         struct Os* os,
                         struct Arena scratch) {
  struct Buf text = {.arena = &scratch};
  gallery_config_print_key_string(&text, "title", gallery_config->title);
  gallery_config_print_key_string(&text, "author", gallery_config->author);
  gallery_config_print_key_string(&text, "base_url", gallery_config->base_url);
  gallery_config_print_key_string(&text, "input_dir", gallery_config->input_dir);
  gallery_config_print_key_string(&text, "output_dir", gallery_config->output_dir);
  gallery_config_print_key_string(&text, "templates_dir", gallery_config->templates_dir);
  gallery_config_print_key_string(&text, "static_dir", gallery_config->static_dir);
  gallery_config_print_key_string(&text, "album_template", gallery_config->album_template);
  gallery_config_print_string_array(&text, "aggregate_templates",
                                    gallery_config->aggregate_templates,
                                    gallery_config->aggregate_template_count);
  buf_append(&text, S("derivatives = { "));
  for (ptrdiff_t i = 0; i < gallery_config->derivative_count; i++) {
    const struct GalleryDerivative* derivative = &gallery_config->derivatives[i];
    if (i > 0) {
      buf_append(&text, S(", "));
    }
    buf_printf(&text, "%.*s = { width = %td, height = %td, quality = %td",
               STR_FMT(derivative->name), derivative->width_px, derivative->height_px,
               derivative->quality);
    if (derivative->is_crop) {
      buf_append(&text, S(", crop = true"));
    }
    buf_append(&text, S(" }"));
  }
  buf_append(&text, S(" }\n"));
  gallery_config_print_sort(&text, gallery_config);
  buf_printf(&text, "video_frame_seconds = %td\n", gallery_config->video_frame_seconds);
  return os_print(os, 1, buf_str(&text));
}

struct Str gallery_config_derivative_recipe(const struct GalleryConfig* gallery_config,
                                            struct Arena* arena) {
  struct Buf recipe = {.arena = arena};
  buf_printf(&recipe, "v%d", DERIVATIVE_RECIPE_VERSION);
  for (ptrdiff_t i = 0; i < gallery_config->derivative_count; i++) {
    const struct GalleryDerivative* derivative = &gallery_config->derivatives[i];
    buf_printf(&recipe, "-%.*s-%tdx%td%s-q%td", STR_FMT(derivative->name), derivative->width_px,
               derivative->height_px, derivative->is_crop ? "c" : "", derivative->quality);
  }
  buf_printf(&recipe, "-f%td", gallery_config->video_frame_seconds * 1000);
  return buf_str(&recipe);
}

static int gallery_config_load_toml(struct Str config_path,
                                    struct Os* os,
                                    struct Arena scratch,
                                    toml_result_t* parsed_out,
                                    struct Log* log) {
  struct Str config_data = {0};
  if (os_read(os, config_path, CONFIG_FILE_LEN_MAX, &scratch, &config_data,
              S("failed to read config"), log) != 0) {
    return -1;
  }

  if (!text_is_nul_free(config_data)) {
    return log_error(log, "failed to read config: contains an embedded NUL byte ('%.*s')",
                     STR_FMT(config_path));
  }
  // tomlc17 copies the input, so the bytes can stay in `scratch`. The read bounds the length at
  // `CONFIG_FILE_LEN_MAX`, which fits an `int`.
  return toml_parse_document(config_data, config_path, parsed_out, S("failed to parse config"),
                             log);
}

static void gallery_config_load_fields(struct GalleryConfig* gallery_config,
                                       toml_datum_t table,
                                       struct Arena* perm,
                                       struct Log* log) {
  // Every key is checked even after one fails, so a config with several mistakes reports all of
  // them in one run. A rejected value is never stored: the key keeps its default, so a build that
  // goes on to check its media does so against a usable config rather than reporting the same
  // mistake again once per media file.
  (void)toml_require_known_keys(table, config_keys, sizeof(config_keys) / sizeof(config_keys[0]),
                                S("config"), S(""), (struct Str){0}, log);

  const struct GalleryConfigStringKey strings[] = {
      {"title", &gallery_config->title, true},
      {"author", &gallery_config->author, true},
      {"base_url", &gallery_config->base_url, false},
      {"album_template", &gallery_config->album_template, false},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(strings) / sizeof(strings[0])); i++) {
    (void)copy_string(table, strings[i].key, strings[i].is_required, perm, strings[i].field, log);
  }

  (void)normalize_base_url(gallery_config, log);
  gallery_config->is_input_dir_rejected =
      copy_dir(table, "input_dir", INPUT_DIR_DEFAULT, perm, &gallery_config->input_dir, log) != 0;
  gallery_config->is_output_dir_rejected = copy_dir(table, "output_dir", OUTPUT_DIR_DEFAULT, perm,
                                                    &gallery_config->output_dir, log) != 0;
  gallery_config->is_templates_dir_rejected =
      copy_dir(table, "templates_dir", TEMPLATES_DIR_DEFAULT, perm, &gallery_config->templates_dir,
               log) != 0;
  gallery_config->is_static_dir_rejected = copy_dir(table, "static_dir", STATIC_DIR_DEFAULT, perm,
                                                    &gallery_config->static_dir, log) != 0;
  (void)normalize_album_template(gallery_config, log);
  (void)copy_template_names(table, "aggregate_templates", perm,
                            &gallery_config->aggregate_templates,
                            &gallery_config->aggregate_template_count, log);
  (void)copy_derivatives(gallery_config, table, perm, log);
  (void)copy_sort(gallery_config, table, log);

  const struct GalleryConfigIntKey video_frame_seconds = {
      .key = "video_frame_seconds",
      .field = &gallery_config->video_frame_seconds,
      .value_max = VIDEO_FRAME_SECONDS_MAX,
      .limit_name = "video frame seconds",
  };
  (void)copy_size(table, S(""), &video_frame_seconds, log);
}

static int copy_string(toml_datum_t table,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       struct Str* value_out,
                       struct Log* log) {
  const toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    if (is_required) {
      return log_error(log, "missing required config key '%s'", key);
    }
    return 0;
  }
  if (value.type != TOML_STRING) {
    return log_error(log, "config key '%s' must be a string", key);
  }
  if (!toml_datum_is_text(value)) {
    return log_error(log, "config key '%s' must not contain a NUL byte", key);
  }
  *value_out = str_clone(toml_datum_str(value), arena);
  return 0;
}

static int copy_template_names(toml_datum_t table,
                               const char* key,
                               struct Arena* arena,
                               const struct Str** template_names_out,
                               ptrdiff_t* template_name_count_out,
                               struct Log* log) {
  const toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_ARRAY) {
    return log_error(log, "config key '%s' must be an array", key);
  }

  const ptrdiff_t item_count = value.u.arr.size;
  // This allocates even for an empty array, rather than leaving it `NULL`. A zero-count allocation
  // still returns a pointer. Every consumer of the published array is declared `nonnull` on it. If
  // left `NULL`, `aggregate_templates = []` would reach `gallery_config_print_string_array` as a
  // null pointer and abort.
  struct Str* items = arena_new(arena, item_count, struct Str);

  // Every element is checked even after one fails, so each unsafe name is reported. A wrong type or
  // a `NUL` byte is reported once, since its message names no element and a repeat would add
  // nothing.
  int rc = 0;
  bool is_type_reported = false;
  bool is_nul_reported = false;
  for (ptrdiff_t i = 0; i < item_count; i++) {
    const toml_datum_t item = value.u.arr.elem[i];
    if (item.type != TOML_STRING) {
      if (!is_type_reported) {
        rc = log_error(log, "config key '%s' must contain only strings", key);
        is_type_reported = true;
      }
      continue;
    }
    if (!toml_datum_is_text(item)) {
      if (!is_nul_reported) {
        rc = log_error(log, "config key '%s' must not contain a NUL byte", key);
        is_nul_reported = true;
      }
      continue;
    }
    const struct Str name = toml_datum_str(item);
    if (!path_is_safe_relative(name)) {
      rc = log_error(log, "config key '%s' must contain only safe relative template names: '%.*s'",
                     key, STR_FMT(name));
      continue;
    }
    items[i] = str_clone(name, arena);
  }
  if (rc != 0) {
    return -1;
  }

  // The code fills these through a mutable local and publishes them as `const struct Str*`. The
  // elements have to be writable while it builds the arena copy, but must not be once the config
  // shares them with the render workers.
  *template_names_out = items;
  *template_name_count_out = item_count;
  return 0;
}

static int copy_derivatives(struct GalleryConfig* gallery_config,
                            toml_datum_t table,
                            struct Arena* perm,
                            struct Log* log) {
  const toml_datum_t value = toml_get(table, "derivatives");
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_TABLE) {
    return log_error(log, "config key 'derivatives' must be a table");
  }
  if (value.u.tab.size < 1) {
    return log_error(log, "config key 'derivatives' must not be empty");
  }
  if ((uint32_t)value.u.tab.size > GALLERY_DERIVATIVE_COUNT_MAX) {
    return log_error(log, "config key 'derivatives' exceeds max derivative count (%d) at %" PRId32,
                     GALLERY_DERIVATIVE_COUNT_MAX, value.u.tab.size);
  }

  const ptrdiff_t derivative_count = value.u.tab.size;
  struct GalleryDerivative* derivatives =
      arena_new(perm, derivative_count, struct GalleryDerivative);
  int rc = 0;
  for (ptrdiff_t i = 0; i < derivative_count; i++) {
    const struct Str name = {value.u.tab.key[i], value.u.tab.len[i]};
    if (copy_derivatives_entry(name, value.u.tab.value[i], perm, &derivatives[i], log) != 0) {
      rc = -1;
    }
  }
  // The bounds check reads both dimensions, so it runs only once every one of them was accepted.
  if (rc == 0 && require_derivative_bounds(derivatives, derivative_count, log) != 0) {
    rc = -1;
  }
  if (rc == 0) {
    gallery_config->derivatives = derivatives;
    gallery_config->derivative_count = derivative_count;
  }
  return rc;
}

static int copy_derivatives_entry(struct Str name,
                                  toml_datum_t entry_table,
                                  struct Arena* perm,
                                  struct GalleryDerivative* derivative_out,
                                  struct Log* log) {
  // A quoted key can hold a `NUL` decoded from an escape, where `%.*s` would stop and name a
  // different derivative, so such a name is rejected without being named.
  if (!text_is_nul_free(name)) {
    return log_error(log, "config key 'derivatives' name must not contain a NUL byte");
  }
  if (!is_derivative_name(name)) {
    return log_error(log,
                     "config key 'derivatives' name must match [A-Za-z][A-Za-z0-9_]* and be at "
                     "most %d bytes: '%.*s'",
                     GALLERY_DERIVATIVE_NAME_LEN_MAX, STR_FMT(name));
  }
  derivative_out->name = str_clone(name, perm);

  if (entry_table.type != TOML_TABLE) {
    return log_error(log, "config key 'derivatives.%.*s' must be a table",
                     STR_FMT(derivative_out->name));
  }
  char key_prefix_buffer[sizeof("derivatives..") + GALLERY_DERIVATIVE_NAME_LEN_MAX];
  const int key_prefix_len = snprintf(key_prefix_buffer, sizeof(key_prefix_buffer),
                                      "derivatives.%.*s.", STR_FMT(derivative_out->name));
  const struct Str key_prefix = {key_prefix_buffer, key_prefix_len};
  int rc = 0;
  if (toml_require_known_keys(entry_table, derivative_keys,
                              sizeof(derivative_keys) / sizeof(derivative_keys[0]), S("config"),
                              key_prefix, (struct Str){0}, log) != 0) {
    rc = -1;
  }
  const struct GalleryConfigIntKey integers[] = {
      {"width", &derivative_out->width_px, 1, IMAGE_ENCODED_DIMENSION_MAX, "WebP dimension", true},
      {"height", &derivative_out->height_px, 1, IMAGE_ENCODED_DIMENSION_MAX, "WebP dimension",
       true},
      {"quality", &derivative_out->quality, 1, 100, "WebP quality", true},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(integers) / sizeof(integers[0])); i++) {
    if (copy_size(entry_table, key_prefix, &integers[i], log) != 0) {
      rc = -1;
    }
  }

  const toml_datum_t crop = toml_get(entry_table, "crop");
  if (crop.type != TOML_UNKNOWN && crop.type != TOML_BOOLEAN) {
    rc = log_error(log, "config key 'derivatives.%.*s.crop' must be a boolean",
                   STR_FMT(derivative_out->name));
  }
  derivative_out->is_crop = crop.type == TOML_BOOLEAN && crop.u.boolean;
  return rc;
}

static bool is_derivative_name(struct Str name) {
  if (name.len == 0 || name.len > GALLERY_DERIVATIVE_NAME_LEN_MAX ||
      !((name.data[0] >= 'A' && name.data[0] <= 'Z') ||
        (name.data[0] >= 'a' && name.data[0] <= 'z'))) {
    return false;
  }
  for (ptrdiff_t i = 1; i < name.len; i++) {
    if (!ascii_is_alphanumeric((unsigned char)name.data[i]) && name.data[i] != '_') {
      return false;
    }
  }
  return true;
}

static int copy_size(toml_datum_t table,
                     struct Str key_prefix,
                     const struct GalleryConfigIntKey* spec,
                     struct Log* log) {
  const toml_datum_t value = toml_get(table, spec->key);
  if (value.type == TOML_UNKNOWN) {
    if (spec->is_required) {
      return log_error(log, "missing required config key '%.*s%s'", STR_FMT(key_prefix), spec->key);
    }
    return 0;
  }
  if (value.type != TOML_INT64) {
    return log_error(log, "config key '%.*s%s' must be an integer", STR_FMT(key_prefix), spec->key);
  }
  if (value.u.int64 < spec->value_min) {
    return log_error(log, "config key '%.*s%s' must be at least %td: %" PRId64, STR_FMT(key_prefix),
                     spec->key, spec->value_min, value.u.int64);
  }
  // One comparison covers both the configured ceiling and the conversion below. `value_max` is a
  // `ptrdiff_t`, so a value this check accepts fits `ptrdiff_t` on every target.
  if (value.u.int64 > spec->value_max) {
    return log_error(log, "config key '%.*s%s' exceeds max %s (%td) at %" PRId64,
                     STR_FMT(key_prefix), spec->key, spec->limit_name, spec->value_max,
                     value.u.int64);
  }
  *spec->field = (ptrdiff_t)value.u.int64;
  return 0;
}

static int copy_sort(struct GalleryConfig* gallery_config, toml_datum_t table, struct Log* log) {
  const toml_datum_t value = toml_get(table, "sort");
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_TABLE) {
    return log_error(log, "config key 'sort' must be a table");
  }
  int rc = 0;
  if (toml_require_known_keys(value, sort_keys, sizeof(sort_keys) / sizeof(sort_keys[0]),
                              S("config"), S("sort."), (struct Str){0}, log) != 0) {
    rc = -1;
  }
  if (copy_sort_order(value, "albums", &gallery_config->album_sort_order, log) != 0) {
    rc = -1;
  }
  if (copy_sort_order(value, "media", &gallery_config->media_sort_order, log) != 0) {
    rc = -1;
  }
  return rc;
}

static int copy_sort_order(toml_datum_t sort_table,
                           const char* key,
                           enum GallerySortOrder* order_out,
                           struct Log* log) {
  const toml_datum_t value = toml_get(sort_table, key);
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_STRING) {
    return log_error(log, "config key 'sort.%s' must be a string", key);
  }
  // A `NUL` decoded from an escape would cut the quoted value below, so the message names none.
  if (!toml_datum_is_text(value)) {
    return log_error(log, "config key 'sort.%s' must not contain a NUL byte", key);
  }
  const struct Str order = toml_datum_str(value);
  if (str_eq(order, S("asc"))) {
    *order_out = GALLERY_SORT_ASC;
    return 0;
  }
  if (str_eq(order, S("desc"))) {
    *order_out = GALLERY_SORT_DESC;
    return 0;
  }
  return log_error(log, "config key 'sort.%s' must be 'asc' or 'desc': '%.*s'", key,
                   STR_FMT(order));
}

static int normalize_base_url(struct GalleryConfig* gallery_config, struct Log* log) {
  if (gallery_config->base_url.len == 0) {
    return 0;
  }
  if (!is_valid_base_url(gallery_config->base_url)) {
    const int rc = log_error(
        log,
        "config key 'base_url' must be an absolute 'http://' or 'https://' URL with a host: '%.*s'",
        STR_FMT(gallery_config->base_url));
    gallery_config->base_url = (struct Str){0};
    return rc;
  }

  // Trim a trailing `/` so templates can join `base_url` with a path that starts with one without
  // producing `//`. Normalizing once here keeps the rule in one place and makes
  // `gallery_config_print` round-trip the normalized form, as `copy_dir` does for directories. The
  // trim cannot eat the scheme's slashes, because the check above proved a host byte follows them.
  // The value is a counted view, so trimming shortens it in place.
  gallery_config->base_url = trim_trailing_slashes(gallery_config->base_url);
  return 0;
}

static bool is_valid_base_url(struct Str url) {
  /**
   * It accepts a scheme plus a non-empty host, and nothing more. A port, subpath, or query string
   * must stay valid, and judging a host beyond "not empty" would mean ruling on what DNS and
   * punycode allow. Schemes are case-insensitive, so `HTTPS://` is the same URL.
   */
  static const struct Str URL_SCHEMES[] = {STR_INIT("http://"), STR_INIT("https://")};

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(URL_SCHEMES) / sizeof(URL_SCHEMES[0])); i++) {
    if (has_scheme(url, URL_SCHEMES[i])) {
      // `https://` has no host at all. `https:///path`, `https://?q`, `https://#f` and
      // `https://:8080` have an empty host: `/`, `?`, `#`, or the port's `:` starts immediately.
      const struct Str host = str_tail(url, URL_SCHEMES[i].len);
      return host.len > 0 && host.data[0] != '/' && host.data[0] != '?' && host.data[0] != '#' &&
             host.data[0] != ':';
    }
  }
  return false;
}

static bool has_scheme(struct Str url, struct Str scheme) {
  if (url.len < scheme.len) {
    return false;
  }
  for (ptrdiff_t i = 0; i < scheme.len; i++) {
    if (ascii_to_lower((unsigned char)url.data[i]) !=
        ascii_to_lower((unsigned char)scheme.data[i])) {
      return false;
    }
  }
  return true;
}

static int copy_dir(toml_datum_t table,
                    const char* key,
                    struct Str fallback,
                    struct Arena* arena,
                    struct Str* value_out,
                    struct Log* log) {
  if (copy_string(table, key, false, arena, value_out, log) != 0) {
    return -1;
  }
  *value_out = trim_trailing_slashes(*value_out);
  if (value_out->len == 0) {
    *value_out = fallback;
    return log_error(log, "config key '%s' must not be empty", key);
  }
  return 0;
}

static struct Str trim_trailing_slashes(struct Str value) {
  while (str_endswith(value, S("/"))) {
    value = str_head(value, value.len - 1);
  }
  return value;
}

static int normalize_album_template(struct GalleryConfig* gallery_config, struct Log* log) {
  if (path_is_safe_relative(gallery_config->album_template)) {
    return 0;
  }
  const int rc =
      log_error(log, "config key 'album_template' must be a safe relative template name: '%.*s'",
                STR_FMT(gallery_config->album_template));
  gallery_config->album_template = ALBUM_TEMPLATE_DEFAULT;
  return rc;
}

static int require_derivative_bounds(const struct GalleryDerivative* derivatives,
                                     ptrdiff_t derivative_count,
                                     struct Log* log) {
  int rc = 0;
  for (ptrdiff_t i = 0; i < derivative_count; i++) {
    const struct GalleryDerivative* derivative = &derivatives[i];
    if (derivative->height_px > IMAGE_PIXEL_COUNT_MAX / derivative->width_px) {
      rc = log_error(log,
                     "config key 'derivatives.%.*s' dimensions exceed max pixel count (%d pixels) "
                     "at %td x %td",
                     STR_FMT(derivative->name), IMAGE_PIXEL_COUNT_MAX, derivative->width_px,
                     derivative->height_px);
    }
  }
  return rc;
}

static void gallery_config_print_key_string(struct Buf* buffer, const char* key, struct Str value) {
  buf_printf(buffer, "%s = ", key);
  print_string(buffer, value);
  buf_append_char(buffer, '\n');
}

static void gallery_config_print_string_array(struct Buf* buffer,
                                              const char* key,
                                              const struct Str* items,
                                              ptrdiff_t item_count) {
  buf_printf(buffer, "%s = [", key);
  for (ptrdiff_t i = 0; i < item_count; i++) {
    if (i > 0) {
      buf_append(buffer, S(", "));
    }
    print_string(buffer, items[i]);
  }
  buf_append(buffer, S("]\n"));
}

static void gallery_config_print_sort(struct Buf* buffer,
                                      const struct GalleryConfig* gallery_config) {
  buf_printf(buffer, "sort = { albums = \"%s\", media = \"%s\" }\n",
             gallery_config->album_sort_order == GALLERY_SORT_DESC ? "desc" : "asc",
             gallery_config->media_sort_order == GALLERY_SORT_DESC ? "desc" : "asc");
}

static void print_string(struct Buf* buffer, struct Str text) {
  buf_append_char(buffer, '"');
  // Walk the bytes as `unsigned char`. Where `char` is signed, every byte of a multi-byte UTF-8
  // sequence is negative, so the `< 0x20` test below would treat it as a control byte, and passing
  // a negative `int` to the `%04X` conversion is undefined.
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const unsigned char c = (unsigned char)text.data[i];
    switch (c) {
      case '"':
        buf_append(buffer, S("\\\""));
        break;
      case '\\':
        buf_append(buffer, S("\\\\"));
        break;
      case '\b':
        buf_append(buffer, S("\\b"));
        break;
      case '\t':
        buf_append(buffer, S("\\t"));
        break;
      case '\n':
        buf_append(buffer, S("\\n"));
        break;
      case '\f':
        buf_append(buffer, S("\\f"));
        break;
      case '\r':
        buf_append(buffer, S("\\r"));
        break;
      default:
        if (c < 0x20 || c == 0x7F) {
          buf_printf(buffer, "\\u%04X", c);
        } else {
          buf_append_char(buffer, (char)c);
        }
        break;
    }
  }
  buf_append_char(buffer, '"');
}
