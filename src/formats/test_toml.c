#include <acutest.h>
#include <stdio.h>
#include <string.h>
#include <tomlc17.h>

#include "core/error.h"
#include "formats/toml.h"

/**
 * @brief Parses a one-value TOML document.
 *
 * @param value      TOML value text assigned to key `x`.
 * @param result_out Parse result the caller must release with `toml_free`.
 * @return The datum bound to `x`, borrowing storage from `result_out`.
 */
static toml_datum_t parse_value(const char* value, toml_result_t* result_out) {
  char doc[128];
  const int n = snprintf(doc, sizeof(doc), "x = %s\n", value);
  TEST_ASSERT(n > 0 && (size_t)n < sizeof(doc));
  *result_out = toml_parse_named(doc, n, "test");
  TEST_ASSERT(result_out->ok);
  return toml_get(result_out->toptab, "x");
}

// A plain string is text. A string carrying a `NUL` decoded from an escape is not. Every caller
// that turns a TOML string into an owned C string checks this first, so a datum that passes here is
// one `strlen` can measure. Without the escape case the check would pass for the wrong reason,
// since no `NUL` can appear literally in a TOML document.
static void test_is_text_rejects_embedded_nul(void) {
  toml_result_t result;
  TEST_CHECK(toml_datum_is_text(parse_value("\"plain\"", &result)));
  toml_free(result);

  TEST_CHECK(!toml_datum_is_text(parse_value("\"pre\\u0000post\"", &result)));
  toml_free(result);

  // A non-string datum is not text either, so no caller reaches the copy path with an integer.
  TEST_CHECK(!toml_datum_is_text(parse_value("7", &result)));
  toml_free(result);
}

// A table holding only listed keys passes, whatever their order, and so does an empty table checked
// against an empty list.
static void test_require_known_keys_accepts_known_keys(void) {
  static const char* const known_keys[] = {"quality", "crop"};
  char err[ERROR_MESSAGE_SIZE] = "";
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ crop = 1, quality = 2 }", &result), known_keys,
                                     sizeof(known_keys) / sizeof(known_keys[0]), "config", "", err,
                                     sizeof(err)) == 0);
  toml_free(result);

  TEST_CHECK(toml_require_known_keys(parse_value("{}", &result), NULL, 0, "config", "", err,
                                     sizeof(err)) == 0);
  toml_free(result);
}

// An unlisted key is rejected, named with the caller's key kind and table prefix. Keys match
// exactly, so a prefix of a known key, or one extending it, is as unknown as a typo, and an empty
// list rejects every key.
static void test_require_known_keys_rejects_unknown_keys(void) {
  static const char* const known_keys[] = {"quality", "crop"};
  static const char* const cases[][4] = {
      {"{ quality = 1, qaulity = 2 }", "config", "", "unknown config key 'qaulity'"},
      {"{ corp = true }", "config", "derivatives.m.", "unknown config key 'derivatives.m.corp'"},
      {"{ qualit = 1 }", "derivative", "", "unknown derivative key 'qualit'"},
      {"{ quality_max = 1 }", "derivative", "", "unknown derivative key 'quality_max'"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char err[ERROR_MESSAGE_SIZE] = "";
    toml_result_t result;
    TEST_CHECK(toml_require_known_keys(parse_value(cases[i][0], &result), known_keys,
                                       sizeof(known_keys) / sizeof(known_keys[0]), cases[i][1],
                                       cases[i][2], err, sizeof(err)) == -1);
    TEST_CHECK(strcmp(err, cases[i][3]) == 0);
    TEST_MSG("case %zu: got '%s'", i, err);
    toml_free(result);
  }

  char err[ERROR_MESSAGE_SIZE] = "";
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ title = 1 }", &result), NULL, 0, "config", "",
                                     err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, "unknown config key 'title'") == 0);
  toml_free(result);
}

TEST_LIST = {
    {"is text rejects embedded nul", test_is_text_rejects_embedded_nul},
    {"require known keys accepts known keys", test_require_known_keys_accepts_known_keys},
    {"require known keys rejects unknown keys", test_require_known_keys_rejects_unknown_keys},
    {NULL, NULL},
};
