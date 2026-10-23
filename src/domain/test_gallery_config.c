#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/error.h"
#include "domain/gallery_config.h"
#include "formats/image.h"
#include "runtime/fs.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * Size of a buffer for an expected diagnostic. The log never truncates, so this only has to hold
 * the longest message a test spells out.
 */
enum { EXPECTED_SIZE = 8192 };

/** Fixture root template for a config written by `write_temp_config`. */
#define TEMP_CONFIG_ROOT_TEMPLATE "/tmp/fram-gallery-config-test.XXXXXX"

/** A `fram.toml` written alone into a fresh fixture root. */
struct TempConfig {
  /** Fixture root, which `remove_fixture_tree` removes together with the config. */
  char root_dir[sizeof(TEMP_CONFIG_ROOT_TEMPLATE)];

  /** Path of the written config. */
  char path[sizeof(TEMP_CONFIG_ROOT_TEMPLATE) + sizeof("/fram.toml") - 1];
};

/**
 * @brief Loads a config with a fresh long-lived arena and a fresh scratch arena.
 *
 * Each call reserves its own two arenas, so the loaded values stay valid for the rest of the test
 * and never overlap the scratch the load worked in.
 *
 * @param gallery_config Initialized config that receives the values. Must not be `NULL`.
 * @param config_path    Terminated path of the config to load. Must not be `NULL`.
 * @param log            Log that receives every error the load records. Must not be `NULL`.
 * @return `0` when the config was loaded with no error, or `-1` when it could not be read or
 *         parsed, or any key was rejected. `gallery_config_load` itself returns `0` in the last
 *         case, which `test_load_returns_zero_with_key_errors` pins.
 */
static int load_gallery_config(struct GalleryConfig* gallery_config,
                               const char* config_path,
                               struct Log* log) {
  struct Arena perm = test_arena();
  const int rc = gallery_config_load(gallery_config, str_from_cstr(config_path), os_posix(), &perm,
                                     test_arena(), log);
  return rc != 0 || log->errors > 0 ? -1 : 0;
}

/**
 * @brief Renders a gallery configuration into a terminated text buffer.
 *
 * `gallery_config_print` writes to standard output, so its descriptor is captured while it runs.
 *
 * @param gallery_config Configuration to render.
 * @param text_out       Buffer that receives the rendered text.
 * @param text_out_len   Size of `text_out` in bytes. Must be non-zero.
 * @return `0` on success, or `-1` after recording a test failure.
 */
static int render(const struct GalleryConfig* gallery_config,
                  char* text_out,
                  ptrdiff_t text_out_len) {
  text_out[0] = '\0';
  struct StreamCapture capture;
  if (capture_begin(stdout, &capture) != 0) {
    return -1;
  }
  const bool is_printed =
      TEST_CHECK(gallery_config_print(gallery_config, os_posix(), test_arena()) == 0);
  const bool is_captured = capture_end(&capture, text_out, text_out_len) == 0;
  return is_printed && is_captured ? 0 : -1;
}

/**
 * @brief Writes TOML to `fram.toml` in a fresh fixture root.
 *
 * On success the caller removes the root with `remove_fixture_tree(temp_config_out->root_dir)`. On
 * failure no fixture is left behind.
 *
 * @param temp_config_out Receives the fixture root and the config path.
 * @param toml            Terminated TOML text to write.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int write_temp_config(struct TempConfig* temp_config_out, const char* toml) {
  memcpy(temp_config_out->root_dir, TEMP_CONFIG_ROOT_TEMPLATE, sizeof(TEMP_CONFIG_ROOT_TEMPLATE));
  if (init_fixture_dir(temp_config_out->root_dir) != 0) {
    return -1;
  }
  const int n = snprintf(temp_config_out->path, sizeof(temp_config_out->path), "%s/fram.toml",
                         temp_config_out->root_dir);
  if (!TEST_CHECK(n > 0 && (size_t)n < sizeof(temp_config_out->path))) {
    remove_fixture_tree(temp_config_out->root_dir);
    return -1;
  }
  if (!TEST_CHECK(write_fixture_file(temp_config_out->root_dir, "fram.toml", toml) == 0)) {
    remove_fixture_tree(temp_config_out->root_dir);
    return -1;
  }
  return 0;
}

/**
 * @brief Loads a config with the given extra TOML and checks it fails with one diagnostic.
 *
 * @param toml_extra Terminated TOML appended to the required keys of an otherwise valid config.
 * @param expected   Exact diagnostic `gallery_config_load` must report.
 */
static void check_load_rejects(const char* toml_extra, const char* expected) {
  char toml[4096];
  const int n = snprintf(toml, sizeof(toml),
                         "title = \"Example Gallery\"\n"
                         "author = \"Example Author\"\n"
                         "%s",
                         toml_extra);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, expected));

  remove_fixture_tree(temp_config.root_dir);
}

// Loading a file with only the required keys fills the rest from `gallery_config_init`'s defaults.
static void test_load_applies_required_and_defaults(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == 0);

  TEST_CHECK(str_eq(config.title, S("Example Gallery")));
  TEST_CHECK(str_eq(config.author, S("Example Author")));
  TEST_CHECK(str_eq(config.base_url, S("")));
  TEST_CHECK(str_eq(config.input_dir, S("media")));
  TEST_CHECK(str_eq(config.output_dir, S("public")));
  TEST_CHECK(str_eq(config.templates_dir, S("templates")));
  TEST_CHECK(str_eq(config.static_dir, S("static")));
  TEST_CHECK(str_eq(config.album_template, S("album.html")));
  TEST_CHECK(config.aggregate_templates != NULL);
  TEST_CHECK(config.aggregate_template_count == 0);
  if (!TEST_CHECK(config.derivative_count == 3)) {
    goto cleanup;
  }
  TEST_CHECK(str_eq(config.derivatives[0].name, S("s")));
  TEST_CHECK(config.derivatives[0].width_px == 120);
  TEST_CHECK(config.derivatives[0].height_px == 120);
  TEST_CHECK(config.derivatives[0].quality == 70);
  TEST_CHECK(config.derivatives[0].is_crop);
  TEST_CHECK(str_eq(config.derivatives[1].name, S("m")));
  TEST_CHECK(config.derivatives[1].width_px == 520);
  TEST_CHECK(config.derivatives[1].height_px == 360);
  TEST_CHECK(config.derivatives[1].quality == 80);
  TEST_CHECK(config.derivatives[1].is_crop);
  TEST_CHECK(str_eq(config.derivatives[2].name, S("l")));
  TEST_CHECK(config.derivatives[2].width_px == 792);
  TEST_CHECK(config.derivatives[2].height_px == 594);
  TEST_CHECK(config.derivatives[2].quality == 85);
  TEST_CHECK(!config.derivatives[2].is_crop);
  TEST_CHECK(config.album_sort_order == GALLERY_SORT_ASC);
  TEST_CHECK(config.media_sort_order == GALLERY_SORT_ASC);
  TEST_CHECK(config.video_frame_seconds == 1);

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// Loading a file that sets every optional key overrides each corresponding default.
static void test_load_overrides_optional_keys(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https://example.com\"\n"
      "input_dir = \"photos\"\n"
      "output_dir = \"dist\"\n"
      "templates_dir = \"layouts\"\n"
      "static_dir = \"files\"\n"
      "album_template = \"pages/album.html\"\n"
      "aggregate_templates = [\"sitemap.xml\", \"about/index.html\"]\n"
      "derivatives = { tiny = { width = 200, height = 150, quality = 61, crop = true }, "
      "display = { width = 1200, height = 900, quality = 93 } }\n"
      "sort = { albums = \"desc\", media = \"desc\" }\n"
      "video_frame_seconds = 17\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == 0);

  TEST_CHECK(str_eq(config.base_url, S("https://example.com")));
  TEST_CHECK(str_eq(config.input_dir, S("photos")));
  TEST_CHECK(str_eq(config.output_dir, S("dist")));
  TEST_CHECK(str_eq(config.templates_dir, S("layouts")));
  TEST_CHECK(str_eq(config.static_dir, S("files")));
  TEST_CHECK(str_eq(config.album_template, S("pages/album.html")));
  if (!TEST_CHECK(config.aggregate_template_count == 2)) {
    goto cleanup;
  }
  TEST_CHECK(str_eq(config.aggregate_templates[0], S("sitemap.xml")));
  TEST_CHECK(str_eq(config.aggregate_templates[1], S("about/index.html")));
  if (!TEST_CHECK(config.derivative_count == 2)) {
    goto cleanup;
  }
  TEST_CHECK(str_eq(config.derivatives[0].name, S("tiny")));
  TEST_CHECK(config.derivatives[0].width_px == 200);
  TEST_CHECK(config.derivatives[0].height_px == 150);
  TEST_CHECK(config.derivatives[0].quality == 61);
  TEST_CHECK(config.derivatives[0].is_crop);
  TEST_CHECK(str_eq(config.derivatives[1].name, S("display")));
  TEST_CHECK(config.derivatives[1].width_px == 1200);
  TEST_CHECK(config.derivatives[1].height_px == 900);
  TEST_CHECK(config.derivatives[1].quality == 93);
  TEST_CHECK(!config.derivatives[1].is_crop);
  TEST_CHECK(config.album_sort_order == GALLERY_SORT_DESC);
  TEST_CHECK(config.media_sort_order == GALLERY_SORT_DESC);
  TEST_CHECK(config.video_frame_seconds == 17);

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// Directory keys have trailing separators trimmed at load. Left in place, a trailing slash defeats
// the `<input_dir>/` prefix strip that places each media item in its album. The end-to-end
// consequence is asserted in the `cmd_build` tests. An absolute or parent-relative directory
// survives normalization unchanged.
static void test_load_normalizes_directory_keys(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "author = \"Example Author\"\n"
      "input_dir = \"media/\"\n"
      "output_dir = \"/srv/www///\"\n"
      "templates_dir = \"../shared/templates/\"\n"
      "static_dir = \"files/\"\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(str_eq(config.input_dir, S("media")));
  TEST_CHECK(str_eq(config.output_dir, S("/srv/www")));
  TEST_CHECK(str_eq(config.templates_dir, S("../shared/templates")));
  TEST_CHECK(str_eq(config.static_dir, S("files")));

  remove_fixture_tree(temp_config.root_dir);
}

// An absolute `base_url` keeps its scheme, host, port, path and query, and loses only a trailing
// `/`, which templates would otherwise double when joining it with a path.
static void test_load_normalizes_base_url(void) {
  static const char* const cases[][2] = {
      {"https://example.com", "https://example.com"},
      {"https://example.com/", "https://example.com"},
      {"https://example.com///", "https://example.com"},
      {"http://example.com/gallery/", "http://example.com/gallery"},
      {"https://example.com:8443/gallery?draft=1", "https://example.com:8443/gallery?draft=1"},
      {"HTTPS://Example.COM", "HTTPS://Example.COM"},  // the scheme matches case-insensitively
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "title = \"Example Gallery\"\n"
                           "author = \"Example Author\"\n"
                           "base_url = \"%s\"\n",
                           cases[i][0]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, toml) != 0) {
      continue;
    }

    struct GalleryConfig config;
    gallery_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == 0);
    TEST_CHECK(log.errors == 0);
    TEST_CHECK(str_eq(config.base_url, str_from_cstr(cases[i][1])));

    remove_fixture_tree(temp_config.root_dir);
  }
}

// Overlapping directories load, because comparing path text cannot tell whether two directories
// overlap. The build compares them by identity before it walks or writes anything.
static void test_load_leaves_directory_overlap_to_the_build(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "author = \"Example Author\"\n"
      "input_dir = \"photos\"\n"
      "output_dir = \"photos/generated\"\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(str_eq(config.input_dir, S("photos")));
  TEST_CHECK(str_eq(config.output_dir, S("photos/generated")));

  remove_fixture_tree(temp_config.root_dir);
}

// Each `sort` key is read on its own. Each case sets the two keys to different orders, so a load
// that filled one field from the other key would fail one of them.
static void test_load_reads_sort_orders_independently(void) {
  static const struct {
    const char* toml_extra;
    enum GallerySortOrder album_sort_order;
    enum GallerySortOrder media_sort_order;
  } cases[] = {
      {"[sort]\nalbums = \"desc\"\nmedia = \"asc\"\n", GALLERY_SORT_DESC, GALLERY_SORT_ASC},
      {"[sort]\nalbums = \"asc\"\nmedia = \"desc\"\n", GALLERY_SORT_ASC, GALLERY_SORT_DESC},
      {"[sort]\nmedia = \"desc\"\n", GALLERY_SORT_ASC, GALLERY_SORT_DESC},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    char toml[256];
    TEST_ASSERT(
        snprintf(toml, sizeof(toml), "title = \"T\"\nauthor = \"A\"\n%s", cases[i].toml_extra) > 0);
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, toml) != 0) {
      continue;
    }
    struct GalleryConfig config;
    gallery_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == 0);
    TEST_CHECK(config.album_sort_order == cases[i].album_sort_order);
    TEST_CHECK(config.media_sort_order == cases[i].media_sort_order);
    TEST_MSG("case %td", i);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// An empty `[sort]` table is accepted and keeps both defaults, unlike an empty `derivatives` table,
// which is rejected.
static void test_load_accepts_empty_sort_table(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "author = \"Example Author\"\n"
      "[sort]\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == 0);
  TEST_CHECK(config.album_sort_order == GALLERY_SORT_ASC);
  TEST_CHECK(config.media_sort_order == GALLERY_SORT_ASC);

  remove_fixture_tree(temp_config.root_dir);
}

// Zero is accepted, which is the boundary below every other accepting case in this file. A guard of
// `< 1` on `video_frame_seconds` would reject a legal config while still passing all of them. Zero
// takes each video's frame at its start.
static void test_load_accepts_zero_video_frame_seconds(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "author = \"Example Author\"\n"
      "video_frame_seconds = 0\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(config.video_frame_seconds == 0);

  remove_fixture_tree(temp_config.root_dir);
}

// Loading a config path that does not exist reports a read failure naming the path and cause.
static void test_load_rejects_missing_file(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  const char* missing_path = "/tmp/fram-gallery-config-test-missing.toml";
  TEST_CHECK(load_gallery_config(&config, missing_path, &log) == -1);
  // The cause distinguishes a missing file from an unreadable or malformed one. Derived from the
  // running libc rather than hardcoded, so the assertion is the whole claim and stays portable.
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('%s')",
               error_system_message(reason, sizeof(reason), ENOENT), missing_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(test_log_is_one(&log, expected));
}

// A config larger than `CONFIG_FILE_LEN_MAX` is rejected at the read, from its size, naming the
// limit and the size. The constant is file-local to `gallery_config.c`, so the 1 MiB below is
// spelled out and must change with it. The file is sparse, so the fixture costs no disk.
static void test_load_rejects_oversize_file(void) {
  enum { CONFIG_FILE_LEN_MAX = 1024 * 1024 };
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, "title = \"Gallery\"\n") != 0) {
    return;
  }
  const off_t config_len = (off_t)CONFIG_FILE_LEN_MAX + 1;
  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read config: exceeds max file size (%d bytes) at %jd bytes ('%s')",
               CONFIG_FILE_LEN_MAX, (intmax_t)config_len, temp_config.path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  if (!TEST_CHECK(truncate(temp_config.path, config_len) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// A config file holding an embedded `NUL` is rejected before the parse, naming the config path.
// `os_read` accepts binary data because it also reads media, so the config boundary itself
// establishes the `NUL`-free text invariant.
static void test_load_rejects_nul_in_file(void) {
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, "") != 0) {
    return;
  }
  static const char config_data[] = "title = \"Gallery\"\0author = \"Example Author\"\n";
  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read config: contains an embedded NUL byte ('%s')", temp_config.path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  if (!TEST_CHECK(os_write(os_posix(), str_from_cstr(temp_config.path),
                           (struct OsSource){.bytes = {config_data, sizeof(config_data) - 1}},
                           S("failed to write fixture"), &log) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// `gallery_config_load` reports a syntactically malformed config as a parse failure naming the
// config path, rather than reaching the field pass with an empty table and reporting a false
// missing-key error.
static void test_load_rejects_malformed_toml(void) {
  const char toml[] = "title = \n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
  // The prefix and the trailing attribution are this module's own contribution. The text between
  // them is the vendored parser's `errmsg`, left unpinned so rewording it upstream does not fail
  // this test. The two together are still the whole first-party claim, which neither the
  // missing-key message nor the read-failure message could satisfy.
  const struct Str prefix = S("failed to parse config: ");
  const struct Str message = buf_str(&log.buf);
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(str_startswith(message, prefix));
  char suffix[EXPECTED_SIZE];
  const int n = snprintf(suffix, sizeof(suffix), " (in '%s')\n", temp_config.path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(suffix));
  // A cause of at least one byte has to sit between the two, or the parser reported nothing.
  TEST_CHECK(message.len > prefix.len + n);
  TEST_CHECK(str_endswith(message, (struct Str){suffix, n}));

  remove_fixture_tree(temp_config.root_dir);
}

/**
 * A config that parses but holds seven independent mistakes, one each for the unknown-key check, a
 * string key's type, `base_url`, a directory, `album_template`, a derivative, and
 * `video_frame_seconds`.
 */
static const char TOML_KEYS_INVALID[] =
    "title = 1\n"
    "author = \"Example Author\"\n"
    "base_url = \"ftp://example.com\"\n"
    "colour = \"blue\"\n"
    "input_dir = \"/\"\n"
    "album_template = \"../album.html\"\n"
    "derivatives = { s = { width = 0, height = 120, quality = 70 } }\n"
    "video_frame_seconds = -1\n";

// Every invalid key in one config is reported in one load, in the order the load checks them, not
// only the first.
static void test_load_reports_every_invalid_key(void) {
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, TOML_KEYS_INVALID) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "unknown config key 'colour'\n"
                         "config key 'title' must be a string\n"
                         "config key 'base_url' must be an absolute 'http://' or 'https://' URL "
                         "with a host: 'ftp://example.com'\n"
                         "config key 'input_dir' must not be empty\n"
                         "config key 'album_template' must be a safe relative template name: "
                         "'../album.html'\n"
                         "config key 'derivatives.s.width' must be at least 1: 0\n"
                         "config key 'video_frame_seconds' must be at least 0: -1\n"));
  TEST_CHECK(log.errors == 7);

  remove_fixture_tree(temp_config.root_dir);
}

// A config that was read and parsed loads successfully even when keys are rejected: the errors are
// in the log, and the result only says whether there was a file to check.
static void test_load_returns_zero_with_key_errors(void) {
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, TOML_KEYS_INVALID) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  struct Arena perm = test_arena();
  TEST_CHECK(gallery_config_load(&config, str_from_cstr(temp_config.path), os_posix(), &perm,
                                 test_arena(), &log) == 0);
  TEST_CHECK(log.errors == 7);

  remove_fixture_tree(temp_config.root_dir);
}

// A rejected value is never stored. Each rejected key keeps the default `gallery_config_init` gave
// it, so later checks run against a usable config, while the valid keys around them still load. A
// derivative table with one bad entry keeps the default derivatives whole. A rejected directory key
// is marked as such.
static void test_load_keeps_defaults_for_rejected_keys(void) {
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, TOML_KEYS_INVALID) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(str_eq(config.input_dir, S("media")));
  TEST_CHECK(config.is_input_dir_rejected);
  TEST_CHECK(!config.is_output_dir_rejected);
  TEST_CHECK(!config.is_templates_dir_rejected);
  TEST_CHECK(!config.is_static_dir_rejected);
  TEST_CHECK(str_eq(config.album_template, S("album.html")));
  TEST_CHECK(config.base_url.len == 0);
  TEST_CHECK(config.derivative_count == 3);
  TEST_CHECK(config.derivatives[0].width_px == 120);
  TEST_CHECK(config.video_frame_seconds == 1);
  TEST_CHECK(config.title.len == 0);
  TEST_CHECK(str_eq(config.author, S("Example Author")));

  remove_fixture_tree(temp_config.root_dir);
}

// Omitting a required key is reported as missing rather than as a type error.
static void test_load_rejects_missing_required_key(void) {
  const char toml[] = "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, "missing required config key 'title'"));

  remove_fixture_tree(temp_config.root_dir);
}

// A required key of the wrong TOML type is reported as a type error, not a missing key.
static void test_load_rejects_wrong_key_type(void) {
  const char toml[] =
      "title = 123\n"
      "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, "config key 'title' must be a string"));

  remove_fixture_tree(temp_config.root_dir);
}

// A key the schema does not define is rejected, naming the key, so a misspelling cannot silently
// keep a default. A key that is a prefix of a known key, or extends one, is as unknown as any
// other, and so is a table the schema has no place for. A key in a derivative table is named by its
// full dotted path.
static void test_load_rejects_unknown_keys(void) {
  static const char* const cases[][2] = {
      {"title = \"Example Gallery\"\nauthor = \"Example Author\"\nouput_dir = \"site\"\n",
       "unknown config key 'ouput_dir'"},
      {"titl = \"Example Gallery\"\ntitle = \"Example Gallery\"\nauthor = \"Example Author\"\n",
       "unknown config key 'titl'"},
      {"title = \"Example Gallery\"\nauthor = \"Example Author\"\ntitles = \"Example Gallery\"\n",
       "unknown config key 'titles'"},
      {"title = \"Example Gallery\"\nauthor = \"Example Author\"\n[extra]\nkey = 1\n",
       "unknown config key 'extra'"},
      {"title = \"Example Gallery\"\nauthor = \"Example Author\"\n"
       "[derivatives.m]\nwidth = 10\nheight = 10\nquality = 80\ncorp = true\n",
       "unknown config key 'derivatives.m.corp'"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, cases[i][0]) != 0) {
      continue;
    }
    struct GalleryConfig config;
    gallery_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));
    TEST_MSG("case %td", i);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// Only `derivatives` and `sort` hold a table, so a table under any other known key fails that key's
// type check.
static void test_load_rejects_table_values(void) {
  static const char* const cases[][2] = {
      {"author = \"Example Author\"\n[title]\nname = \"Example Gallery\"\n",
       "config key 'title' must be a string"},
      {"title = \"Example Gallery\"\nauthor = \"Example Author\"\n"
       "[aggregate_templates]\nname = \"sitemap.xml\"\n",
       "config key 'aggregate_templates' must be an array"},
      {"title = \"Example Gallery\"\nauthor = \"Example Author\"\n"
       "[video_frame_seconds]\nseconds = 1\n",
       "config key 'video_frame_seconds' must be an integer"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, cases[i][0]) != 0) {
      continue;
    }
    struct GalleryConfig config;
    gallery_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));
    TEST_MSG("case %td", i);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// An integer key rejects a TOML type other than integer and enforces its exact lower bound, which
// is 1 for a derivative dimension or quality and 0 for `video_frame_seconds`. A non-integer fails a
// different assertion from an out-of-range one: the user has to change the type, not the number.
static void test_load_rejects_invalid_integer_type_and_minimum(void) {
  check_load_rejects("derivatives = { s = { width = \"320\", height = 120, quality = 70 } }\n",
                     "config key 'derivatives.s.width' must be an integer");
  check_load_rejects("derivatives = { s = { width = 0, height = 120, quality = 70 } }\n",
                     "config key 'derivatives.s.width' must be at least 1: 0");
  check_load_rejects("derivatives = { s = { width = 120, height = 120, quality = 0 } }\n",
                     "config key 'derivatives.s.quality' must be at least 1: 0");
  check_load_rejects("video_frame_seconds = -1\n",
                     "config key 'video_frame_seconds' must be at least 0: -1");
}

// A TOML escape decoding to `U+0000` is rejected per key. tomlc17 accepts the escape and returns a
// string whose bytes carry an embedded `NUL`, so this is a separate boundary from the file check:
// the file itself holds no `NUL`. The byte has no meaning in any text format `fram` reads or
// writes, so the load refuses it.
static void test_load_rejects_nul_in_string_values(void) {
  static const char* const cases[][2] = {
      {"title = \"a\\u0000b\"\nauthor = \"A\"\n", "config key 'title' must not contain a NUL byte"},
      {"title = \"T\"\nauthor = \"A\"\nbase_url = \"a\\u0000b\"\n",
       "config key 'base_url' must not contain a NUL byte"},
      {"title = \"T\"\nauthor = \"A\"\ninput_dir = \"a\\u0000b\"\n",
       "config key 'input_dir' must not contain a NUL byte"},
      {"title = \"T\"\nauthor = \"A\"\naggregate_templates = [\"a\\u0000b\"]\n",
       "config key 'aggregate_templates' must not contain a NUL byte"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, cases[i][0]) != 0) {
      continue;
    }

    struct GalleryConfig config;
    gallery_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));

    remove_fixture_tree(temp_config.root_dir);
  }
}

// A directory key that is empty, or that trims to empty, is rejected by name.
static void test_load_rejects_empty_directory_keys(void) {
  static const char* const cases[][2] = {
      {"input_dir = \"\"\n", "config key 'input_dir' must not be empty"},
      {"output_dir = \"/\"\n", "config key 'output_dir' must not be empty"},
      {"templates_dir = \"///\"\n", "config key 'templates_dir' must not be empty"},
      {"static_dir = \"\"\n", "config key 'static_dir' must not be empty"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    check_load_rejects(cases[i][0], cases[i][1]);
  }
}

// `gallery_config_load` rejects a present `base_url` that is not an absolute HTTP(S) URL. Without
// that rejection it is not a load error at all: it silently produces a broken link wherever a
// template builds an absolute URL from it. An empty `base_url` means the key is absent, so it is
// not among the cases.
static void test_load_rejects_relative_base_url(void) {
  static const char* const base_urls_invalid[] = {
      "example.com",         // no scheme
      "/relative",           // a path, not a URL
      "ftp://example.com",   // a scheme, but not one a browser follows from a page
      "https:/example.com",  // one slash short of a scheme
      "https://",            // scheme with no host
      "https:///path",       // the host ends before it starts
      "https://?draft=1",    // likewise
      "https://:8080",       // a port with no host before it
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(base_urls_invalid) / sizeof(base_urls_invalid[0]));
       i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "title = \"Example Gallery\"\n"
                           "author = \"Example Author\"\n"
                           "base_url = \"%s\"\n",
                           base_urls_invalid[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, toml) != 0) {
      continue;
    }

    struct GalleryConfig config;
    gallery_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
    char expected[EXPECTED_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'base_url' must be an absolute 'http://' or 'https://' URL with "
                 "a host: '%s'",
                 base_urls_invalid[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(test_log_is_one(&log, expected));

    remove_fixture_tree(temp_config.root_dir);
  }
}

// An unsafe `album_template` is rejected at load, in the singular wording that names the key,
// rather than flowing through to the render and failing once per album. Parent-relative, absolute
// and empty names are all unsafe.
static void test_load_rejects_unsafe_album_template(void) {
  const char* const unsafe_names[] = {"../evil.html", "/etc/passwd", ""};

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(unsafe_names) / sizeof(unsafe_names[0])); i++) {
    char toml_extra[256];
    const int n =
        snprintf(toml_extra, sizeof(toml_extra), "album_template = \"%s\"\n", unsafe_names[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_extra));
    char expected[EXPECTED_SIZE];
    const int expected_len = snprintf(
        expected, sizeof(expected),
        "config key 'album_template' must be a safe relative template name: '%s'", unsafe_names[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    check_load_rejects(toml_extra, expected);
  }
}

// A non-array value, a non-string element, and an unsafe name for a template array key are each
// rejected.
static void test_load_rejects_invalid_template_arrays(void) {
  check_load_rejects("aggregate_templates = \"sitemap.xml\"\n",
                     "config key 'aggregate_templates' must be an array");
  check_load_rejects("aggregate_templates = [1]\n",
                     "config key 'aggregate_templates' must contain only strings");
  check_load_rejects("aggregate_templates = [\"../evil.html\"]\n",
                     "config key 'aggregate_templates' must contain only safe relative template "
                     "names: '../evil.html'");
}

// Every element of a template array is checked even after one fails, so each unsafe name is
// reported in element order. A wrong type or a `NUL` byte is reported once however many elements
// have it, since its message names no element. The rejected key keeps its default.
static void test_load_reports_every_invalid_template_name(void) {
  struct TempConfig temp_config;
  if (write_temp_config(
          &temp_config,
          "title = \"Example Gallery\"\n"
          "author = \"Example Author\"\n"
          "aggregate_templates = [\"../a.xml\", 1, \"a\\u0000b\", \"ok.xml\", \"/b.xml\", "
          "2, \"c\\u0000d\"]\n") != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  const struct Str* const aggregate_templates_default = config.aggregate_templates;
  const ptrdiff_t aggregate_template_count_default = config.aggregate_template_count;
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "config key 'aggregate_templates' must contain only safe relative "
                         "template names: '../a.xml'\n"
                         "config key 'aggregate_templates' must contain only strings\n"
                         "config key 'aggregate_templates' must not contain a NUL byte\n"
                         "config key 'aggregate_templates' must contain only safe relative "
                         "template names: '/b.xml'\n"));
  TEST_CHECK(log.errors == 4);
  TEST_CHECK(config.aggregate_templates == aggregate_templates_default);
  TEST_CHECK(config.aggregate_template_count == aggregate_template_count_default);

  remove_fixture_tree(temp_config.root_dir);
}

// Dimension, quality, and video-frame upper bounds are exact and named in diagnostics.
static void test_load_rejects_integer_upper_bounds(void) {
  char toml_extra[256];
  TEST_ASSERT(snprintf(toml_extra, sizeof(toml_extra),
                       "derivatives = { l = { width = %d, height = 1, quality = 85 } }\n",
                       IMAGE_ENCODED_DIMENSION_MAX + 1) > 0);
  char expected[EXPECTED_SIZE];
  TEST_ASSERT(snprintf(expected, sizeof(expected),
                       "config key 'derivatives.l.width' exceeds max WebP dimension (%d) at %d",
                       IMAGE_ENCODED_DIMENSION_MAX, IMAGE_ENCODED_DIMENSION_MAX + 1) > 0);
  check_load_rejects(toml_extra, expected);

  check_load_rejects("derivatives = { l = { width = 1, height = 1, quality = 101 } }\n",
                     "config key 'derivatives.l.quality' exceeds max WebP quality (100) at 101");
  // `VIDEO_FRAME_SECONDS_MAX` is file-local to `gallery_config.c`, so the one day below is spelled
  // out and must change with it.
  check_load_rejects("video_frame_seconds = 86401\n",
                     "config key 'video_frame_seconds' exceeds max video frame seconds (86400) at "
                     "86401");
}

// Even individually valid dimensions cannot describe a box beyond the decoder's pixel bound.
static void test_load_rejects_dimension_product_overflow(void) {
  char toml_extra[256];
  TEST_ASSERT(snprintf(toml_extra, sizeof(toml_extra),
                       "derivatives = { huge = { width = %d, height = %d, quality = 85 } }\n",
                       IMAGE_ENCODED_DIMENSION_MAX, IMAGE_ENCODED_DIMENSION_MAX) > 0);
  char expected[EXPECTED_SIZE];
  TEST_ASSERT(snprintf(expected, sizeof(expected),
                       "config key 'derivatives.huge' dimensions exceed max pixel count (%d "
                       "pixels) at %d x %d",
                       IMAGE_PIXEL_COUNT_MAX, IMAGE_ENCODED_DIMENSION_MAX,
                       IMAGE_ENCODED_DIMENSION_MAX) > 0);
  check_load_rejects(toml_extra, expected);
}

// Derivative tables require bounded identifiers, dimensions, and boolean crop flags. A name that
// holds a `NUL` byte is rejected without being named, since printing it would stop at the `NUL`.
static void test_load_rejects_invalid_derivative_tables(void) {
  check_load_rejects("derivatives = { s = { width = 120, quality = 70 } }\n",
                     "missing required config key 'derivatives.s.height'");
  check_load_rejects("derivatives = { s = { width = 120, height = 120 } }\n",
                     "missing required config key 'derivatives.s.quality'");
  char expected_name[EXPECTED_SIZE];
  TEST_ASSERT(snprintf(expected_name, sizeof(expected_name),
                       "config key 'derivatives' name must match [A-Za-z][A-Za-z0-9_]* and be at "
                       "most %d bytes: 'not.safe'",
                       GALLERY_DERIVATIVE_NAME_LEN_MAX) > 0);
  check_load_rejects("derivatives = { \"not.safe\" = { width = 1, height = 1 } }\n", expected_name);
  check_load_rejects("derivatives = { \"a\\u0000b\" = { width = 1, height = 1, quality = 1 } }\n",
                     "config key 'derivatives' name must not contain a NUL byte");
  check_load_rejects("derivatives = { s = { width = 1, height = 1, quality = 70, crop = 1 } }\n",
                     "config key 'derivatives.s.crop' must be a boolean");
  char expected_count[EXPECTED_SIZE];
  TEST_ASSERT(snprintf(expected_count, sizeof(expected_count),
                       "config key 'derivatives' exceeds max derivative count (%d) at %d",
                       GALLERY_DERIVATIVE_COUNT_MAX, GALLERY_DERIVATIVE_COUNT_MAX + 1) > 0);
  check_load_rejects(
      "derivatives = { a = { width = 1, height = 1, quality = 1 }, "
      "b = { width = 1, height = 1, quality = 1 }, "
      "c = { width = 1, height = 1, quality = 1 }, "
      "d = { width = 1, height = 1, quality = 1 }, "
      "e = { width = 1, height = 1, quality = 1 }, "
      "f = { width = 1, height = 1, quality = 1 }, "
      "g = { width = 1, height = 1, quality = 1 } }\n",
      expected_count);
}

// The `sort` table must be a table, holds only its two keys, and each key takes exactly the
// lowercase string `asc` or `desc`. A case variant, a longer word, and an empty string are each
// rejected and named. A value holding a `NUL` byte is rejected without being named, since printing
// it would stop at the `NUL`.
static void test_load_rejects_invalid_sort_tables(void) {
  check_load_rejects("sort = \"desc\"\n", "config key 'sort' must be a table");
  check_load_rejects("sort = { album = \"desc\" }\n", "unknown config key 'sort.album'");
  check_load_rejects("sort = { media = 1 }\n", "config key 'sort.media' must be a string");
  check_load_rejects("sort = { albums = \"ASC\" }\n",
                     "config key 'sort.albums' must be 'asc' or 'desc': 'ASC'");
  check_load_rejects("sort = { media = \"descending\" }\n",
                     "config key 'sort.media' must be 'asc' or 'desc': 'descending'");
  check_load_rejects("sort = { media = \"\" }\n",
                     "config key 'sort.media' must be 'asc' or 'desc': ''");
  check_load_rejects("sort = { albums = \"a\\u0000sc\" }\n",
                     "config key 'sort.albums' must not contain a NUL byte");
}

// Every invalid key in a `sort` table is reported in one load: the unknown key first, then each
// order in schema order. A rejected order is never stored, so a field the caller had already set
// keeps that value rather than falling back to `asc`.
static void test_load_reports_every_invalid_sort_key(void) {
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config,
                        "title = \"Example Gallery\"\n"
                        "author = \"Example Author\"\n"
                        "sort = { albums = \"up\", order = \"asc\", media = 1 }\n") != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  config.album_sort_order = GALLERY_SORT_DESC;
  config.media_sort_order = GALLERY_SORT_DESC;
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "unknown config key 'sort.order'\n"
                         "config key 'sort.albums' must be 'asc' or 'desc': 'up'\n"
                         "config key 'sort.media' must be a string\n"));
  TEST_CHECK(log.errors == 3);
  TEST_CHECK(config.album_sort_order == GALLERY_SORT_DESC);
  TEST_CHECK(config.media_sort_order == GALLERY_SORT_DESC);

  remove_fixture_tree(temp_config.root_dir);
}

// Printing emits every key, with the defaults `gallery_config_init` supplies for the optional ones.
// `title` and `author` have no default and are set here so the output shows each. A derivative
// prints `crop` only when it crops.
static void test_print_defaults(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  config.title = S("Example Gallery");
  config.author = S("Example Author");

  char config_out[1024];
  TEST_ASSERT(render(&config, config_out, sizeof(config_out)) == 0);

  // The whole buffer, not a set of per-line searches: only an exact comparison can catch a key that
  // is missing, duplicated, extra, or emitted in the wrong order.
  TEST_CHECK(strcmp(config_out,
                    "title = \"Example Gallery\"\n"
                    "author = \"Example Author\"\n"
                    "base_url = \"\"\n"
                    "input_dir = \"media\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "static_dir = \"static\"\n"
                    "album_template = \"album.html\"\n"
                    "aggregate_templates = []\n"
                    "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
                    "m = { width = 520, height = 360, quality = 80, crop = true }, "
                    "l = { width = 792, height = 594, quality = 85 } }\n"
                    "sort = { albums = \"asc\", media = \"asc\" }\n"
                    "video_frame_seconds = 1\n") == 0);
}

// Printing escapes quotes, backslashes, and every named control escape (\t \b \n \f \r). It uses
// the `\uXXXX` fallback for a sub-`0x20` byte and `U+007F` (`DEL`).
static void test_print_escapes_strings(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  config.title = S("Quote \" and \\ backslash");
  // Each hex escape is followed by a non-hex-digit char ('Z', 'G') so it ends after one byte.
  config.author = S("a\tA\bB\nC\fD\rE\x01Z\x7FG");

  char config_out[1024];
  TEST_ASSERT(render(&config, config_out, sizeof(config_out)) == 0);

  // Compared whole, so an escape leaking into a neighboring field cannot hide.
  TEST_CHECK(strcmp(config_out,
                    "title = \"Quote \\\" and \\\\ backslash\"\n"
                    "author = \"a\\tA\\bB\\nC\\fD\\rE\\u0001Z\\u007FG\"\n"
                    "base_url = \"\"\n"
                    "input_dir = \"media\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "static_dir = \"static\"\n"
                    "album_template = \"album.html\"\n"
                    "aggregate_templates = []\n"
                    "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
                    "m = { width = 520, height = 360, quality = 80, crop = true }, "
                    "l = { width = 792, height = 594, quality = 85 } }\n"
                    "sort = { albums = \"asc\", media = \"asc\" }\n"
                    "video_frame_seconds = 1\n") == 0);
}

// Printing a config and loading the printed output back yields the same field values, exercising
// the round-trip invariant documented on `gallery_config_print` across escaped strings (including a
// `U+007F` byte), multi-element template arrays, cropping and non-cropping derivatives, and two
// different sort orders.
static void test_print_load_round_trips(void) {
  static const struct Str aggregates[] = {STR_INIT("sitemap.xml"), STR_INIT("about/index.html")};
  static const struct GalleryDerivative derivatives[] = {
      {.name = STR_INIT("tiny"), .width_px = 200, .height_px = 150, .quality = 61, .is_crop = true},
      {.name = STR_INIT("display"),
       .width_px = 1200,
       .height_px = 900,
       .quality = 93,
       .is_crop = false},
  };

  struct GalleryConfig config;
  gallery_config_init(&config);
  // '!' after `\x7F` is not a hex digit, so the escape ends after the single `DEL` byte.
  config.title = S("Quote \" back\\slash\nnewline\ttab\x7F!");
  config.author = S("Example Author");
  config.base_url = S("https://example.com/gallery");
  config.input_dir = S("photos");
  config.output_dir = S("dist");
  config.templates_dir = S("layouts");
  config.static_dir = S("files");
  config.album_template = S("pages/album.html");
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 2;
  config.derivatives = derivatives;
  config.derivative_count = 2;
  config.album_sort_order = GALLERY_SORT_DESC;
  config.media_sort_order = GALLERY_SORT_ASC;
  config.video_frame_seconds = 0;

  char config_out[2048];
  TEST_ASSERT(render(&config, config_out, sizeof(config_out)) == 0);

  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, config_out) != 0) {
    return;
  }

  struct GalleryConfig reloaded;
  gallery_config_init(&reloaded);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&reloaded, temp_config.path, &log) == 0);

  TEST_CHECK(str_eq(reloaded.title, config.title));
  TEST_CHECK(str_eq(reloaded.author, config.author));
  TEST_CHECK(str_eq(reloaded.base_url, config.base_url));
  TEST_CHECK(str_eq(reloaded.input_dir, config.input_dir));
  TEST_CHECK(str_eq(reloaded.output_dir, config.output_dir));
  TEST_CHECK(str_eq(reloaded.templates_dir, config.templates_dir));
  TEST_CHECK(str_eq(reloaded.static_dir, config.static_dir));
  TEST_CHECK(str_eq(reloaded.album_template, config.album_template));
  if (!TEST_CHECK(reloaded.aggregate_template_count == 2)) {
    goto cleanup;
  }
  TEST_CHECK(str_eq(reloaded.aggregate_templates[0], S("sitemap.xml")));
  TEST_CHECK(str_eq(reloaded.aggregate_templates[1], S("about/index.html")));
  if (!TEST_CHECK(reloaded.derivative_count == 2)) {
    goto cleanup;
  }
  for (ptrdiff_t i = 0; i < 2; i++) {
    TEST_CHECK(str_eq(reloaded.derivatives[i].name, derivatives[i].name));
    TEST_CHECK(reloaded.derivatives[i].width_px == derivatives[i].width_px);
    TEST_CHECK(reloaded.derivatives[i].height_px == derivatives[i].height_px);
    TEST_CHECK(reloaded.derivatives[i].quality == derivatives[i].quality);
    TEST_CHECK(reloaded.derivatives[i].is_crop == derivatives[i].is_crop);
  }
  TEST_CHECK(reloaded.album_sort_order == config.album_sort_order);
  TEST_CHECK(reloaded.media_sort_order == config.media_sort_order);
  TEST_CHECK(reloaded.video_frame_seconds == config.video_frame_seconds);

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// An empty template array survives a load followed by a print. Neither operation alone tests this
// sequence. If `copy_template_names` stored `NULL` for `[]`, it would pass that value to the
// `nonnull` function `gallery_config_print_string_array`. Then `fram config` would abort for a
// config that the loader accepted.
static void test_print_empty_template_array(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "author = \"Example Author\"\n"
      "aggregate_templates = []\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_gallery_config(&config, temp_config.path, &log) == 0);
  TEST_CHECK(log.errors == 0);
  if (!TEST_CHECK(config.aggregate_template_count == 0)) {
    goto cleanup;
  }
  // The empty array is a distinct non-`NULL` pointer, which the `nonnull` consumers need.
  TEST_CHECK(config.aggregate_templates != NULL);

  char config_out[1024];
  if (render(&config, config_out, sizeof(config_out)) != 0) {
    goto cleanup;
  }
  // Compared whole: an empty array must print as `[]` and must not disturb its neighbors.
  TEST_CHECK(strcmp(config_out,
                    "title = \"Example Gallery\"\n"
                    "author = \"Example Author\"\n"
                    "base_url = \"\"\n"
                    "input_dir = \"media\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "static_dir = \"static\"\n"
                    "album_template = \"album.html\"\n"
                    "aggregate_templates = []\n"
                    "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
                    "m = { width = 520, height = 360, quality = 80, crop = true }, "
                    "l = { width = 792, height = 594, quality = 85 } }\n"
                    "sort = { albums = \"asc\", media = \"asc\" }\n"
                    "video_frame_seconds = 1\n") == 0);

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// A standard output that refuses the write makes `gallery_config_print` report `-1` with the
// write's own `errno`, which is what lets `cmd_config_run` name a reason instead of reporting
// success over lost output. A read-only descriptor in place of standard output is the deterministic
// way to get one: every write fails with `EBADF`. The same shape is asserted for
// `cli_print_version` by `test_print_version_reports_write_failure`.
static void test_print_reports_write_failure(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  config.title = S("Example Gallery");
  config.author = S("Example Author");

  struct StreamCapture capture;
  if (capture_begin_unwritable(stdout, &capture) != 0) {
    return;
  }
  errno = 0;
  const int rc = gallery_config_print(&config, os_posix(), test_arena());
  const int print_errno = errno;
  TEST_CHECK(capture_end(&capture, NULL, 0) == 0);
  TEST_CHECK(rc == -1);
  TEST_CHECK(print_errno == EBADF);
}

// The recipe component names every output-affecting value: the recipe version, each derivative's
// name, box, crop flag, and quality in configuration order, and the video frame time in
// milliseconds.
static void test_derivative_recipe_encodes_output_values(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Arena arena = test_arena();
  struct Str recipe = gallery_config_derivative_recipe(&config, &arena);
  TEST_CHECK(str_eq(recipe, S("v2-s-120x120c-q70-m-520x360c-q80-l-792x594-q85-f1000")));

  const struct GalleryDerivative custom[] = {
      {.name = STR_INIT("tiny1"), .width_px = 7, .height_px = 8, .quality = 42, .is_crop = true},
      {.name = STR_INIT("screen"),
       .width_px = 90,
       .height_px = 100,
       .quality = 91,
       .is_crop = false},
  };
  config.derivatives = custom;
  config.derivative_count = sizeof(custom) / sizeof(custom[0]);
  config.video_frame_seconds = 0;
  recipe = gallery_config_derivative_recipe(&config, &arena);
  TEST_CHECK(str_eq(recipe, S("v2-tiny1-7x8c-q42-screen-90x100-q91-f0")));
}

TEST_LIST = {
    {"load applies required and defaults", test_load_applies_required_and_defaults},
    {"load overrides optional keys", test_load_overrides_optional_keys},
    {"load normalizes directory keys", test_load_normalizes_directory_keys},
    {"load normalizes base url", test_load_normalizes_base_url},
    {"load leaves directory overlap to the build", test_load_leaves_directory_overlap_to_the_build},
    {"load reads sort orders independently", test_load_reads_sort_orders_independently},
    {"load accepts empty sort table", test_load_accepts_empty_sort_table},
    {"load accepts zero video frame seconds", test_load_accepts_zero_video_frame_seconds},
    {"load rejects missing file", test_load_rejects_missing_file},
    {"load rejects oversize file", test_load_rejects_oversize_file},
    {"load rejects nul in file", test_load_rejects_nul_in_file},
    {"load rejects malformed toml", test_load_rejects_malformed_toml},
    {"load reports every invalid key", test_load_reports_every_invalid_key},
    {"load returns zero with key errors", test_load_returns_zero_with_key_errors},
    {"load keeps defaults for rejected keys", test_load_keeps_defaults_for_rejected_keys},
    {"load rejects missing required key", test_load_rejects_missing_required_key},
    {"load rejects wrong key type", test_load_rejects_wrong_key_type},
    {"load rejects unknown keys", test_load_rejects_unknown_keys},
    {"load rejects table values", test_load_rejects_table_values},
    {"load rejects invalid integer type and minimum",
     test_load_rejects_invalid_integer_type_and_minimum},
    {"load rejects nul in string values", test_load_rejects_nul_in_string_values},
    {"load rejects empty directory keys", test_load_rejects_empty_directory_keys},
    {"load rejects relative base url", test_load_rejects_relative_base_url},
    {"load rejects unsafe album template", test_load_rejects_unsafe_album_template},
    {"load rejects invalid template arrays", test_load_rejects_invalid_template_arrays},
    {"load reports every invalid template name", test_load_reports_every_invalid_template_name},
    {"load rejects integer upper bounds", test_load_rejects_integer_upper_bounds},
    {"load rejects dimension product overflow", test_load_rejects_dimension_product_overflow},
    {"load rejects invalid derivative tables", test_load_rejects_invalid_derivative_tables},
    {"load rejects invalid sort tables", test_load_rejects_invalid_sort_tables},
    {"load reports every invalid sort key", test_load_reports_every_invalid_sort_key},
    {"print defaults", test_print_defaults},
    {"print escapes strings", test_print_escapes_strings},
    {"print load round trips", test_print_load_round_trips},
    {"print empty template array", test_print_empty_template_array},
    {"print reports write failure", test_print_reports_write_failure},
    {"derivative recipe encodes output values", test_derivative_recipe_encodes_output_values},
    {NULL, NULL},
};
