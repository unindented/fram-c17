#include <acutest.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <tomlc17.h>

#include "formats/toml.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

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
  TEST_ASSERT(n > 0 && n < (int)sizeof(doc));
  struct Log log = test_log();
  TEST_ASSERT(toml_parse_document((struct Str){doc, n}, S("test.toml"), result_out,
                                  S("failed to parse test"), &log) == 0);
  return toml_get(result_out->toptab, "x");
}

// A parse error is logged once against the file, after the caller's operation and before tomlc17's
// own message, and the call returns `-1` with nothing left to free. The expected text came from a
// run.
static void test_parse_document_logs_parse_error(void) {
  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_parse_document(S("x = \n"), S("fram.toml"), &result, S("failed to parse config"),
                                 &log) == -1);
  TEST_CHECK(
      test_log_is_one(&log, "failed to parse config: (line 1) missing value (in 'fram.toml')"));
}

// A plain string is text. A string carrying a `NUL` decoded from an escape is not, though its view
// still counts every byte. Every caller that turns a TOML string into a value checks this first.
// Without the escape case the check would pass for the wrong reason, since no `NUL` can appear
// literally in a TOML document.
static void test_is_text_rejects_embedded_nul(void) {
  toml_result_t result;
  toml_datum_t value = parse_value("\"plain\"", &result);
  TEST_CHECK(toml_datum_is_text(value));
  TEST_CHECK(str_eq(toml_datum_str(value), S("plain")));
  toml_free(result);

  value = parse_value("\"pre\\u0000post\"", &result);
  TEST_CHECK(!toml_datum_is_text(value));
  TEST_CHECK(str_eq(toml_datum_str(value), S("pre\0post")));
  toml_free(result);

  // A non-string datum is not text either, so no caller reaches the copy path with an integer.
  TEST_CHECK(!toml_datum_is_text(parse_value("7", &result)));
  toml_free(result);
}

// A table holding only listed keys passes, whatever their order, and so does an empty table checked
// against an empty list. Neither records anything.
static void test_require_known_keys_accepts_known_keys(void) {
  static const struct Str known_keys[] = {STR_INIT("quality"), STR_INIT("crop")};
  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ crop = 1, quality = 2 }", &result), known_keys,
                                     sizeof(known_keys) / sizeof(known_keys[0]), S("config"), S(""),
                                     (struct Str){0}, &log) == 0);
  toml_free(result);

  TEST_CHECK(toml_require_known_keys(parse_value("{}", &result), NULL, 0, S("config"), S(""),
                                     (struct Str){0}, &log) == 0);
  toml_free(result);
  TEST_CHECK(test_log_is_empty(&log));
}

// An unlisted key is rejected, named with the caller's key kind and table prefix. Keys match
// exactly, so a prefix of a known key, or one extending it, is as unknown as a typo, and an empty
// list rejects every key.
static void test_require_known_keys_rejects_unknown_keys(void) {
  static const struct Str known_keys[] = {STR_INIT("quality"), STR_INIT("crop")};
  static const char* const cases[][4] = {
      {"{ quality = 1, qaulity = 2 }", "config", "", "unknown config key 'qaulity'\n"},
      {"{ corp = true }", "config", "derivatives.m.", "unknown config key 'derivatives.m.corp'\n"},
      {"{ qualit = 1 }", "derivative", "", "unknown derivative key 'qualit'\n"},
      {"{ quality_max = 1 }", "derivative", "", "unknown derivative key 'quality_max'\n"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Log log = test_log();
    toml_result_t result;
    TEST_CHECK(toml_require_known_keys(parse_value(cases[i][0], &result), known_keys,
                                       sizeof(known_keys) / sizeof(known_keys[0]),
                                       str_from_cstr(cases[i][1]), str_from_cstr(cases[i][2]),
                                       (struct Str){0}, &log) == -1);
    TEST_CHECK(test_log_is(&log, "%s", cases[i][3]));
    TEST_MSG("case %td", i);
    toml_free(result);
  }

  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ title = 1 }", &result), NULL, 0, S("config"),
                                     S(""), (struct Str){0}, &log) == -1);
  TEST_CHECK(test_log_is(&log, "unknown config key 'title'\n"));
  toml_free(result);
}

// A quoted key holding a `NUL` decoded from an escape is rejected without being named, since `%.*s`
// would stop at the `NUL` and name the known key in front of it. Each such key is still one unknown
// key, beside an ordinary unknown key that keeps its name.
static void test_require_known_keys_rejects_nul_key_unnamed(void) {
  static const struct Str known_keys[] = {STR_INIT("title")};
  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ \"title\\u0000x\" = 1, ttile = 2 }", &result),
                                     known_keys, sizeof(known_keys) / sizeof(known_keys[0]),
                                     S("config"), S(""), S("fram.toml"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "config key must not contain a NUL byte (in 'fram.toml')\n"
                         "unknown config key 'ttile' (in 'fram.toml')\n"));
  TEST_CHECK(log.errors == 2);
  toml_free(result);
}

// A `NUL` key in a nested table is still not named, but its table is, without the `.` of the
// prefix.
static void test_require_known_keys_names_table_of_nul_key(void) {
  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ \"x\\u0000\" = 1 }", &result), NULL, 0,
                                     S("config"), S("derivatives.s."), S("fram.toml"), &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "config key under 'derivatives.s' must not contain a NUL byte (in 'fram.toml')\n"));
  toml_free(result);
}

// Every unknown key is reported, in table order, each naming the file when one is given.
static void test_require_known_keys_reports_every_unknown_key(void) {
  static const struct Str known_keys[] = {STR_INIT("quality")};
  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ qualty = 1, quality = 2, crpo = 3 }", &result),
                                     known_keys, sizeof(known_keys) / sizeof(known_keys[0]),
                                     S("derivative"), S(""), S("fram.toml"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "unknown derivative key 'qualty' (in 'fram.toml')\n"
                         "unknown derivative key 'crpo' (in 'fram.toml')\n"));
  TEST_CHECK(log.errors == 2);
  toml_free(result);
}

TEST_LIST = {
    {"parse document logs parse error", test_parse_document_logs_parse_error},
    {"is text rejects embedded nul", test_is_text_rejects_embedded_nul},
    {"require known keys accepts known keys", test_require_known_keys_accepts_known_keys},
    {"require known keys rejects unknown keys", test_require_known_keys_rejects_unknown_keys},
    {"require known keys rejects nul key unnamed", test_require_known_keys_rejects_nul_key_unnamed},
    {"require known keys names table of nul key", test_require_known_keys_names_table_of_nul_key},
    {"require known keys reports every unknown key",
     test_require_known_keys_reports_every_unknown_key},
    {NULL, NULL},
};
