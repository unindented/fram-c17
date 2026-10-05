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
#include "shared/arena.h"
#include "test_support.h"

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
 * @brief Renders a gallery configuration into a terminated text buffer.
 *
 * @param gallery_config Configuration to render.
 * @param text_out       Buffer that receives the rendered text.
 * @param text_out_len   Size of `text_out` in bytes. Must be non-zero.
 * @return `0` on success, or `-1` after recording a test failure.
 */
static int render(const struct GalleryConfig* gallery_config, char* text_out, size_t text_out_len) {
  FILE* stream = tmpfile();
  TEST_CHECK(stream != NULL);
  if (stream == NULL) {
    return -1;
  }
  const bool is_printed = TEST_CHECK(gallery_config_print(stream, gallery_config) == 0);
  (void)read_capture(stream, text_out, text_out_len);
  const int close_rc = fclose(stream);
  const bool is_closed = TEST_CHECK(close_rc == 0);
  return is_printed && is_closed ? 0 : -1;
}

/**
 * @brief Writes TOML to `fram.toml` in a fresh fixture root.
 *
 * On success the caller removes the root with `remove_fixture_tree(temp_config_out->root_dir)`. On
 * failure no fixture is left behind.
 *
 * @param temp_config_out Receives the fixture root and the config path.
 * @param toml            Terminated TOML text to write.
 * @return The config path, which aliases `temp_config_out->path`, or `NULL` after recording a
 *         test-plumbing failure.
 */
static const char* write_temp_config(struct TempConfig* temp_config_out, const char* toml) {
  memcpy(temp_config_out->root_dir, TEMP_CONFIG_ROOT_TEMPLATE, sizeof(TEMP_CONFIG_ROOT_TEMPLATE));
  if (init_fixture_dir(temp_config_out->root_dir) == NULL) {
    return NULL;
  }
  const int n = snprintf(temp_config_out->path, sizeof(temp_config_out->path), "%s/fram.toml",
                         temp_config_out->root_dir);
  if (!TEST_CHECK(n > 0 && (size_t)n < sizeof(temp_config_out->path))) {
    remove_fixture_tree(temp_config_out->root_dir);
    return NULL;
  }
  if (!TEST_CHECK(write_fixture_file(temp_config_out->root_dir, "fram.toml", toml) == 0)) {
    remove_fixture_tree(temp_config_out->root_dir);
    return NULL;
  }
  return temp_config_out->path;
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
  const char* config_path = write_temp_config(&temp_config, toml);
  if (config_path == NULL) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);

  gallery_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// Loading a file with only the required keys fills the rest from `gallery_config_init`'s defaults.
static void test_load_applies_required_and_defaults(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);
  if (config_path == NULL) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == 0);

  TEST_CHECK(strcmp(config.title, "Example Gallery") == 0);
  TEST_CHECK(strcmp(config.author, "Example Author") == 0);
  TEST_CHECK(strcmp(config.base_url, "") == 0);
  TEST_CHECK(strcmp(config.input_dir, "media") == 0);
  TEST_CHECK(strcmp(config.output_dir, "public") == 0);
  TEST_CHECK(strcmp(config.templates_dir, "templates") == 0);
  TEST_CHECK(strcmp(config.assets_dir, "assets") == 0);
  TEST_CHECK(strcmp(config.album_template, "album.html") == 0);
  TEST_CHECK(config.aggregate_templates != NULL);
  TEST_CHECK(config.aggregate_template_count == 0);
  if (!TEST_CHECK(config.derivative_count == 3)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(config.derivatives[0].name, "s") == 0);
  TEST_CHECK(config.derivatives[0].width_px == 120);
  TEST_CHECK(config.derivatives[0].height_px == 120);
  TEST_CHECK(config.derivatives[0].quality == 70);
  TEST_CHECK(config.derivatives[0].is_crop);
  TEST_CHECK(strcmp(config.derivatives[1].name, "m") == 0);
  TEST_CHECK(config.derivatives[1].width_px == 520);
  TEST_CHECK(config.derivatives[1].height_px == 360);
  TEST_CHECK(config.derivatives[1].quality == 80);
  TEST_CHECK(config.derivatives[1].is_crop);
  TEST_CHECK(strcmp(config.derivatives[2].name, "l") == 0);
  TEST_CHECK(config.derivatives[2].width_px == 792);
  TEST_CHECK(config.derivatives[2].height_px == 594);
  TEST_CHECK(config.derivatives[2].quality == 85);
  TEST_CHECK(!config.derivatives[2].is_crop);
  TEST_CHECK(config.video_frame_seconds == 1);

cleanup:
  gallery_config_free(&config);
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
      "assets_dir = \"static\"\n"
      "album_template = \"pages/album.html\"\n"
      "aggregate_templates = [\"sitemap.xml\", \"about/index.html\"]\n"
      "derivatives = { tiny = { width = 200, height = 150, quality = 61, crop = true }, "
      "display = { width = 1200, height = 900, quality = 93 } }\n"
      "video_frame_seconds = 17\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);
  if (config_path == NULL) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == 0);

  TEST_CHECK(strcmp(config.base_url, "https://example.com") == 0);
  TEST_CHECK(strcmp(config.input_dir, "photos") == 0);
  TEST_CHECK(strcmp(config.output_dir, "dist") == 0);
  TEST_CHECK(strcmp(config.templates_dir, "layouts") == 0);
  TEST_CHECK(strcmp(config.assets_dir, "static") == 0);
  TEST_CHECK(strcmp(config.album_template, "pages/album.html") == 0);
  if (!TEST_CHECK(config.aggregate_template_count == 2)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(config.aggregate_templates[0], "sitemap.xml") == 0);
  TEST_CHECK(strcmp(config.aggregate_templates[1], "about/index.html") == 0);
  if (!TEST_CHECK(config.derivative_count == 2)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(config.derivatives[0].name, "tiny") == 0);
  TEST_CHECK(config.derivatives[0].width_px == 200);
  TEST_CHECK(config.derivatives[0].height_px == 150);
  TEST_CHECK(config.derivatives[0].quality == 61);
  TEST_CHECK(config.derivatives[0].is_crop);
  TEST_CHECK(strcmp(config.derivatives[1].name, "display") == 0);
  TEST_CHECK(config.derivatives[1].width_px == 1200);
  TEST_CHECK(config.derivatives[1].height_px == 900);
  TEST_CHECK(config.derivatives[1].quality == 93);
  TEST_CHECK(!config.derivatives[1].is_crop);
  TEST_CHECK(config.video_frame_seconds == 17);

cleanup:
  gallery_config_free(&config);
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
      "assets_dir = \"static/\"\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);
  if (config_path == NULL) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(strcmp(config.input_dir, "media") == 0);
  TEST_CHECK(strcmp(config.output_dir, "/srv/www") == 0);
  TEST_CHECK(strcmp(config.templates_dir, "../shared/templates") == 0);
  TEST_CHECK(strcmp(config.assets_dir, "static") == 0);

  gallery_config_free(&config);
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

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "title = \"Example Gallery\"\n"
                           "author = \"Example Author\"\n"
                           "base_url = \"%s\"\n",
                           cases[i][0]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, toml);
    if (config_path == NULL) {
      continue;
    }

    struct GalleryConfig config;
    gallery_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == 0);
    TEST_CHECK(err[0] == '\0');
    TEST_CHECK(strcmp(config.base_url, cases[i][1]) == 0);

    gallery_config_free(&config);
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
  const char* config_path = write_temp_config(&temp_config, toml);
  if (config_path == NULL) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(strcmp(config.input_dir, "photos") == 0);
  TEST_CHECK(strcmp(config.output_dir, "photos/generated") == 0);

  gallery_config_free(&config);
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
  const char* config_path = write_temp_config(&temp_config, toml);
  if (config_path == NULL) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(config.video_frame_seconds == 0);

  gallery_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// Loading a config path that does not exist reports a read failure naming the path and cause.
static void test_load_rejects_missing_file(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  const char* missing_path = "/tmp/fram-gallery-config-test-missing.toml";
  TEST_CHECK(gallery_config_load(&config, missing_path, err, sizeof(err)) == -1);
  // The cause distinguishes a missing file from an unreadable or malformed one. Derived from the
  // running libc rather than hardcoded, so the assertion is the whole claim and stays portable.
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('%s')",
               error_system_message(reason, sizeof(reason), ENOENT), missing_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  gallery_config_free(&config);
}

// A config larger than `CONFIG_FILE_LEN_MAX` is rejected at the read, from its size, naming the
// limit and the size. The constant is file-local to `gallery_config.c`, so the 1 MiB below is
// spelled out and must change with it. The file is sparse, so the fixture costs no disk.
static void test_load_rejects_oversize_file(void) {
  enum { CONFIG_FILE_LEN_MAX = 1024 * 1024 };
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, "title = \"Gallery\"\n");
  if (config_path == NULL) {
    return;
  }
  const off_t config_len = (off_t)CONFIG_FILE_LEN_MAX + 1;
  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read config: exceeds max file size (%d bytes) at %jd bytes ('%s')",
               CONFIG_FILE_LEN_MAX, (intmax_t)config_len, config_path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  if (!TEST_CHECK(truncate(config_path, config_len) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  gallery_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// A config file holding an embedded `NUL` is rejected before the parse, naming the config path.
// `fs_read_file` accepts binary data because it also reads media, so the config boundary itself
// establishes the `NUL`-free text invariant.
static void test_load_rejects_nul_in_file(void) {
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, "");
  if (config_path == NULL) {
    return;
  }
  static const char config_data[] = "title = \"Gallery\"\0author = \"Example Author\"\n";
  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read config: contains an embedded NUL byte ('%s')", config_path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  if (!TEST_CHECK(fs_write_file(config_path, config_data, sizeof(config_data) - 1, NULL, 0) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, expected) == 0);

cleanup:
  gallery_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// `gallery_config_load` reports a syntactically malformed config as a parse failure naming the
// config path, rather than reaching the field pass with an empty table and reporting a false
// missing-key error.
static void test_load_rejects_malformed_toml(void) {
  const char toml[] = "title = \n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);
  if (config_path == NULL) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == -1);
  // The prefix and the trailing path are this module's own contribution. The text between them is
  // the vendored parser's `errmsg`, left unpinned so rewording it upstream does not fail this test.
  // The two together are still the whole first-party claim, which neither the missing-key message
  // nor the read-failure message could satisfy.
  const char prefix[] = "failed to parse config: ";
  TEST_CHECK(strncmp(err, prefix, sizeof(prefix) - 1) == 0);
  char suffix[ERROR_MESSAGE_SIZE];
  const int n = snprintf(suffix, sizeof(suffix), " ('%s')", config_path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(suffix));
  const size_t err_len = strlen(err);
  // A cause of at least one byte has to sit between the two, or the parser reported nothing.
  TEST_CHECK(err_len > sizeof(prefix) - 1 + (size_t)n);
  TEST_CHECK(strcmp(err + err_len - (size_t)n, suffix) == 0);

  gallery_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// Omitting a required key is reported as missing rather than as a type error.
static void test_load_rejects_missing_required_key(void) {
  const char toml[] = "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);
  if (config_path == NULL) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, "missing required config key 'title'") == 0);

  gallery_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// A required key of the wrong TOML type is reported as a type error, not a missing key.
static void test_load_rejects_wrong_key_type(void) {
  const char toml[] =
      "title = 123\n"
      "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);
  if (config_path == NULL) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, "config key 'title' must be a string") == 0);

  gallery_config_free(&config);
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

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, cases[i][0]);
    if (config_path == NULL) {
      continue;
    }
    struct GalleryConfig config;
    gallery_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == -1);
    TEST_CHECK(strcmp(err, cases[i][1]) == 0);
    TEST_MSG("case %zu: got '%s'", i, err);
    gallery_config_free(&config);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// Only `derivatives` holds a table, so a table under any other known key fails that key's type
// check.
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

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, cases[i][0]);
    if (config_path == NULL) {
      continue;
    }
    struct GalleryConfig config;
    gallery_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == -1);
    TEST_CHECK(strcmp(err, cases[i][1]) == 0);
    TEST_MSG("case %zu: got '%s'", i, err);
    gallery_config_free(&config);
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
                     "config key 'derivatives.s.width' must be at least 1 at 0");
  check_load_rejects("derivatives = { s = { width = 120, height = 120, quality = 0 } }\n",
                     "config key 'derivatives.s.quality' must be at least 1 at 0");
  check_load_rejects("video_frame_seconds = -1\n",
                     "config key 'video_frame_seconds' must be at least 0 at -1");
}

// A TOML escape decoding to `U+0000` is rejected per key. tomlc17 accepts the escape and returns a
// string whose bytes carry an embedded `NUL`, so this is a separate boundary from the file check:
// the file itself holds no `NUL`. Left through, `strlen` would truncate the value silently.
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

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, cases[i][0]);
    if (config_path == NULL) {
      continue;
    }

    struct GalleryConfig config;
    gallery_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == -1);
    TEST_CHECK(strcmp(err, cases[i][1]) == 0);

    gallery_config_free(&config);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// A directory key that is empty, or that trims to empty, is rejected by name.
static void test_load_rejects_empty_directory_keys(void) {
  static const char* const cases[][2] = {
      {"input_dir = \"\"\n", "config key 'input_dir' must not be empty"},
      {"output_dir = \"/\"\n", "config key 'output_dir' must not be empty"},
      {"templates_dir = \"///\"\n", "config key 'templates_dir' must not be empty"},
      {"assets_dir = \"\"\n", "config key 'assets_dir' must not be empty"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
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
  };

  for (size_t i = 0; i < sizeof(base_urls_invalid) / sizeof(base_urls_invalid[0]); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "title = \"Example Gallery\"\n"
                           "author = \"Example Author\"\n"
                           "base_url = \"%s\"\n",
                           base_urls_invalid[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, toml);
    if (config_path == NULL) {
      continue;
    }

    struct GalleryConfig config;
    gallery_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == -1);
    char expected[ERROR_MESSAGE_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'base_url' must be an absolute 'http://' or 'https://' URL with "
                 "a host: '%s'",
                 base_urls_invalid[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(strcmp(err, expected) == 0);

    gallery_config_free(&config);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// An unsafe `album_template` is rejected at load, in the singular wording that names the key,
// rather than flowing through to the render and failing once per album. Parent-relative, absolute
// and empty names are all unsafe.
static void test_load_rejects_unsafe_album_template(void) {
  const char* const unsafe_names[] = {"../evil.html", "/etc/passwd", ""};

  for (size_t i = 0; i < sizeof(unsafe_names) / sizeof(unsafe_names[0]); i++) {
    char toml_extra[256];
    const int n =
        snprintf(toml_extra, sizeof(toml_extra), "album_template = \"%s\"\n", unsafe_names[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_extra));
    char expected[ERROR_MESSAGE_SIZE];
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

// Dimension, quality, and video-frame upper bounds are exact and named in diagnostics.
static void test_load_rejects_integer_upper_bounds(void) {
  char toml_extra[256];
  TEST_ASSERT(snprintf(toml_extra, sizeof(toml_extra),
                       "derivatives = { l = { width = %d, height = 1, quality = 85 } }\n",
                       IMAGE_DIMENSION_MAX + 1) > 0);
  char expected[ERROR_MESSAGE_SIZE];
  TEST_ASSERT(snprintf(expected, sizeof(expected),
                       "config key 'derivatives.l.width' exceeds max image dimension (%d) at %d",
                       IMAGE_DIMENSION_MAX, IMAGE_DIMENSION_MAX + 1) > 0);
  check_load_rejects(toml_extra, expected);

  check_load_rejects("derivatives = { l = { width = 1, height = 1, quality = 101 } }\n",
                     "config key 'derivatives.l.quality' exceeds max JPEG quality (100) at 101");
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
                       IMAGE_DIMENSION_MAX, IMAGE_DIMENSION_MAX) > 0);
  char expected[ERROR_MESSAGE_SIZE];
  TEST_ASSERT(snprintf(expected, sizeof(expected),
                       "config key 'derivatives.huge' dimensions exceed max image pixel count (%d) "
                       "at %dx%d",
                       IMAGE_PIXEL_COUNT_MAX, IMAGE_DIMENSION_MAX, IMAGE_DIMENSION_MAX) > 0);
  check_load_rejects(toml_extra, expected);
}

// Derivative tables require bounded identifiers, dimensions, and boolean crop flags.
static void test_load_rejects_invalid_derivative_tables(void) {
  check_load_rejects("derivatives = { s = { width = 120, quality = 70 } }\n",
                     "missing required config key 'derivatives.s.height'");
  check_load_rejects("derivatives = { s = { width = 120, height = 120 } }\n",
                     "missing required config key 'derivatives.s.quality'");
  char expected_name[ERROR_MESSAGE_SIZE];
  TEST_ASSERT(snprintf(expected_name, sizeof(expected_name),
                       "config key 'derivatives' name must match [A-Za-z][A-Za-z0-9_]* and be at "
                       "most %d bytes: 'not.safe'",
                       GALLERY_DERIVATIVE_NAME_LEN_MAX) > 0);
  check_load_rejects("derivatives = { \"not.safe\" = { width = 1, height = 1 } }\n", expected_name);
  check_load_rejects("derivatives = { s = { width = 1, height = 1, quality = 70, crop = 1 } }\n",
                     "config key 'derivatives.s.crop' must be a boolean");
  char expected_count[ERROR_MESSAGE_SIZE];
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

// Printing emits every key, with the defaults `gallery_config_init` supplies for the optional ones.
// `title` and `author` have no default and are set here only because `gallery_config_print`
// requires them non-`NULL`. A derivative prints `crop` only when it crops.
static void test_print_defaults(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  config.title = "Example Gallery";
  config.author = "Example Author";

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
                    "assets_dir = \"assets\"\n"
                    "album_template = \"album.html\"\n"
                    "aggregate_templates = []\n"
                    "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
                    "m = { width = 520, height = 360, quality = 80, crop = true }, "
                    "l = { width = 792, height = 594, quality = 85 } }\n"
                    "video_frame_seconds = 1\n") == 0);

  gallery_config_free(&config);
}

// Printing escapes quotes, backslashes, and every named control escape (\t \b \n \f \r). It uses
// the `\uXXXX` fallback for a sub-`0x20` byte and `U+007F` (`DEL`).
static void test_print_escapes_strings(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  config.title = "Quote \" and \\ backslash";
  // Each hex escape is followed by a non-hex-digit char ('Z', 'G') so it ends after one byte.
  config.author = "a\tA\bB\nC\fD\rE\x01Z\x7FG";

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
                    "assets_dir = \"assets\"\n"
                    "album_template = \"album.html\"\n"
                    "aggregate_templates = []\n"
                    "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
                    "m = { width = 520, height = 360, quality = 80, crop = true }, "
                    "l = { width = 792, height = 594, quality = 85 } }\n"
                    "video_frame_seconds = 1\n") == 0);

  gallery_config_free(&config);
}

// Printing a config and loading the printed output back yields the same field values, exercising
// the round-trip invariant documented on `gallery_config_print` across escaped strings (including a
// `U+007F` byte), multi-element template arrays, and cropping and non-cropping derivatives.
static void test_print_load_round_trips(void) {
  static const char* aggregates[] = {"sitemap.xml", "about/index.html"};
  static const struct GalleryDerivative derivatives[] = {
      {.name = "tiny", .width_px = 200, .height_px = 150, .quality = 61, .is_crop = true},
      {.name = "display", .width_px = 1200, .height_px = 900, .quality = 93, .is_crop = false},
  };

  struct GalleryConfig config;
  gallery_config_init(&config);
  // '!' after `\x7F` is not a hex digit, so the escape ends after the single `DEL` byte.
  config.title = "Quote \" back\\slash\nnewline\ttab\x7F!";
  config.author = "Example Author";
  config.base_url = "https://example.com/gallery";
  config.input_dir = "photos";
  config.output_dir = "dist";
  config.templates_dir = "layouts";
  config.assets_dir = "static";
  config.album_template = "pages/album.html";
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 2;
  config.derivatives = derivatives;
  config.derivative_count = 2;
  config.video_frame_seconds = 0;

  char config_out[2048];
  TEST_ASSERT(render(&config, config_out, sizeof(config_out)) == 0);

  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, config_out);
  if (config_path == NULL) {
    gallery_config_free(&config);
    return;
  }

  struct GalleryConfig reloaded;
  gallery_config_init(&reloaded);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(gallery_config_load(&reloaded, config_path, err, sizeof(err)) == 0);

  TEST_CHECK(strcmp(reloaded.title, config.title) == 0);
  TEST_CHECK(strcmp(reloaded.author, config.author) == 0);
  TEST_CHECK(strcmp(reloaded.base_url, config.base_url) == 0);
  TEST_CHECK(strcmp(reloaded.input_dir, config.input_dir) == 0);
  TEST_CHECK(strcmp(reloaded.output_dir, config.output_dir) == 0);
  TEST_CHECK(strcmp(reloaded.templates_dir, config.templates_dir) == 0);
  TEST_CHECK(strcmp(reloaded.assets_dir, config.assets_dir) == 0);
  TEST_CHECK(strcmp(reloaded.album_template, config.album_template) == 0);
  if (!TEST_CHECK(reloaded.aggregate_template_count == 2)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(reloaded.aggregate_templates[0], "sitemap.xml") == 0);
  TEST_CHECK(strcmp(reloaded.aggregate_templates[1], "about/index.html") == 0);
  if (!TEST_CHECK(reloaded.derivative_count == 2)) {
    goto cleanup;
  }
  for (size_t i = 0; i < 2; i++) {
    TEST_CHECK(strcmp(reloaded.derivatives[i].name, derivatives[i].name) == 0);
    TEST_CHECK(reloaded.derivatives[i].width_px == derivatives[i].width_px);
    TEST_CHECK(reloaded.derivatives[i].height_px == derivatives[i].height_px);
    TEST_CHECK(reloaded.derivatives[i].quality == derivatives[i].quality);
    TEST_CHECK(reloaded.derivatives[i].is_crop == derivatives[i].is_crop);
  }
  TEST_CHECK(reloaded.video_frame_seconds == config.video_frame_seconds);

cleanup:
  gallery_config_free(&reloaded);
  gallery_config_free(&config);
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
  const char* config_path = write_temp_config(&temp_config, toml);
  if (config_path == NULL) {
    return;
  }

  struct GalleryConfig config;
  gallery_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(gallery_config_load(&config, config_path, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
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
                    "assets_dir = \"assets\"\n"
                    "album_template = \"album.html\"\n"
                    "aggregate_templates = []\n"
                    "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
                    "m = { width = 520, height = 360, quality = 80, crop = true }, "
                    "l = { width = 792, height = 594, quality = 85 } }\n"
                    "video_frame_seconds = 1\n") == 0);

cleanup:
  gallery_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// A stream that latched a write error makes `gallery_config_print` report `-1` with `errno` set,
// which is what lets `cmd_config_run` name a reason instead of reporting success over truncated
// output. The reason is `EIO` rather than the underlying `EBADF`: a latched error's own `errno` may
// have been overwritten by the time it is noticed, so the contract substitutes a generic I/O
// failure rather than relaying a stale value. A read-mode stream is the deterministic way to reach
// that branch, since the writes fail while `fflush` itself succeeds. The same shape is asserted for
// `cli_print_version` by `test_print_version_reports_write_failure`.
static void test_print_reports_write_failure(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  config.title = "Example Gallery";
  config.author = "Example Author";

  FILE* stream = fopen("/dev/null", "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    gallery_config_free(&config);
    return;
  }
  errno = 0;
  TEST_CHECK(gallery_config_print(stream, &config) == -1);
  TEST_CHECK(errno == EIO);
  const int close_rc = fclose(stream);
  TEST_CHECK(close_rc == 0);

  gallery_config_free(&config);
}

// The recipe component names every output-affecting value: the recipe version, each derivative's
// name, box, crop flag, and quality in configuration order, and the video frame time in
// milliseconds.
static void test_derivative_recipe_encodes_output_values(void) {
  struct GalleryConfig config;
  gallery_config_init(&config);
  struct Arena arena;
  arena_init(&arena);
  char* recipe = gallery_config_derivative_recipe(&config, &arena);
  TEST_ASSERT(recipe != NULL);
  TEST_CHECK(strcmp(recipe, "v1-s-120x120c-q70-m-520x360c-q80-l-792x594-q85-f1000") == 0);

  const struct GalleryDerivative custom[] = {
      {.name = "tiny1", .width_px = 7, .height_px = 8, .quality = 42, .is_crop = true},
      {.name = "screen", .width_px = 90, .height_px = 100, .quality = 91, .is_crop = false},
  };
  config.derivatives = custom;
  config.derivative_count = sizeof(custom) / sizeof(custom[0]);
  config.video_frame_seconds = 0;
  recipe = gallery_config_derivative_recipe(&config, &arena);
  TEST_ASSERT(recipe != NULL);
  TEST_CHECK(strcmp(recipe, "v1-tiny1-7x8c-q42-screen-90x100-q91-f0") == 0);
  arena_free(&arena);
  gallery_config_free(&config);
}

TEST_LIST = {
    {"load applies required and defaults", test_load_applies_required_and_defaults},
    {"load overrides optional keys", test_load_overrides_optional_keys},
    {"load normalizes directory keys", test_load_normalizes_directory_keys},
    {"load normalizes base url", test_load_normalizes_base_url},
    {"load leaves directory overlap to the build", test_load_leaves_directory_overlap_to_the_build},
    {"load accepts zero video frame seconds", test_load_accepts_zero_video_frame_seconds},
    {"load rejects missing file", test_load_rejects_missing_file},
    {"load rejects oversize file", test_load_rejects_oversize_file},
    {"load rejects nul in file", test_load_rejects_nul_in_file},
    {"load rejects malformed toml", test_load_rejects_malformed_toml},
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
    {"load rejects integer upper bounds", test_load_rejects_integer_upper_bounds},
    {"load rejects dimension product overflow", test_load_rejects_dimension_product_overflow},
    {"load rejects invalid derivative tables", test_load_rejects_invalid_derivative_tables},
    {"print defaults", test_print_defaults},
    {"print escapes strings", test_print_escapes_strings},
    {"print load round trips", test_print_load_round_trips},
    {"print empty template array", test_print_empty_template_array},
    {"print reports write failure", test_print_reports_write_failure},
    {"derivative recipe encodes output values", test_derivative_recipe_encodes_output_values},
    {NULL, NULL},
};
