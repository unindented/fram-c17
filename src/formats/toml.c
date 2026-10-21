#include "formats/toml.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "core/text.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"

/**
 * @brief Installs `toml_parse_document_realloc` as tomlc17's allocator. Run once through
 *        `pthread_once`.
 */
static void toml_parse_document_install_allocator(void);

/**
 * @brief Reallocates like `realloc`, calling `os_oom` instead of returning a failure.
 *
 * This is tomlc17's allocator once `toml_parse_document` installs it.
 *
 * @param ptr  Allocation to resize, or `NULL` to allocate.
 * @param size New size in bytes.
 * @return The resized allocation. `NULL` only for a zero `size`, as `realloc` allows.
 */
static void* toml_parse_document_realloc(void* ptr, size_t size);

/**
 * @brief Reports whether a key matches one of a list of names.
 *
 * @param key             Key bytes.
 * @param known_keys      Names to match against. May be `NULL` only when `known_key_count` is 0.
 * @param known_key_count Number of names in `known_keys`.
 * @return `true` when `key` equals one of `known_keys`, otherwise `false`.
 */
static bool is_known_key(struct Str key, const struct Str* known_keys, ptrdiff_t known_key_count);

int toml_parse_document(struct Str document,
                        struct Str file_path,
                        toml_result_t* parsed_out,
                        struct Str operation,
                        struct Log* log) {
  assert(document.len >= 0 && document.len <= INT_MAX);
  static pthread_once_t allocator_once = PTHREAD_ONCE_INIT;
  (void)pthread_once(&allocator_once, toml_parse_document_install_allocator);
  // The source name tomlc17 can attach to each datum goes unread here, and the error message never
  // includes it, so the document is parsed unnamed.
  *parsed_out = toml_parse(document.data, (int)document.len);
  if (!parsed_out->ok) {
    (void)log_error_in(log, file_path, "%.*s: %s", STR_FMT(operation), parsed_out->errmsg);
    // The call currently releases nothing. tomlc17 frees its datum tree and pool on failure, then
    // returns a zeroed result. The vendor requires callers to pass every result to `toml_free`.
    // Following that contract keeps this code correct if a later version retains data after
    // failure.
    toml_free(*parsed_out);
    return -1;
  }
  return 0;
}

bool toml_datum_is_text(toml_datum_t value) {
  // tomlc17 terminates the string but excludes that terminator from `len`, so scanning exactly
  // `len` bytes finds only a `NUL` that the parser decoded from an escape.
  return value.type == TOML_STRING && text_is_nul_free(toml_datum_str(value));
}

struct Str toml_datum_str(toml_datum_t value) {
  assert(value.type == TOML_STRING);
  return (struct Str){value.u.str.ptr, value.u.str.len};
}

int toml_require_known_keys(toml_datum_t table,
                            const struct Str* known_keys,
                            ptrdiff_t known_key_count,
                            struct Str key_kind,
                            struct Str key_prefix,
                            struct Str file_path,
                            struct Log* log) {
  int rc = 0;
  for (int32_t i = 0; i < table.u.tab.size; i++) {
    const struct Str key = {table.u.tab.key[i], table.u.tab.len[i]};
    if (is_known_key(key, known_keys, known_key_count)) {
      continue;
    }
    // A quoted key can hold a `NUL` decoded from an escape, where `%.*s` would stop and name a
    // different key, so such a key is rejected without being named, but with the table it is in. No
    // known key holds a `NUL`, so it is unknown too.
    if (!text_is_nul_free(key) && key_prefix.len > 0) {
      // The table is named without the `.` that joins the prefix to a key.
      const struct Str table_name = str_head(key_prefix, key_prefix.len - 1);
      rc = log_error_in(log, file_path, "%.*s key under '%.*s' must not contain a NUL byte",
                        STR_FMT(key_kind), STR_FMT(table_name));
    } else if (!text_is_nul_free(key)) {
      rc = log_error_in(log, file_path, "%.*s key must not contain a NUL byte", STR_FMT(key_kind));
    } else {
      rc = log_error_in(log, file_path, "unknown %.*s key '%.*s%.*s'", STR_FMT(key_kind),
                        STR_FMT(key_prefix), STR_FMT(key));
    }
  }
  return rc;
}

static void toml_parse_document_install_allocator(void) {
  toml_option_t option = toml_default_option();
  option.mem_realloc = toml_parse_document_realloc;
  toml_set_option(option);
}

static void* toml_parse_document_realloc(void* ptr, size_t size) {
  void* result = realloc(ptr, size);
  if (result == NULL && size > 0) {
    os_oom();
  }
  return result;
}

static bool is_known_key(struct Str key, const struct Str* known_keys, ptrdiff_t known_key_count) {
  for (ptrdiff_t i = 0; i < known_key_count; i++) {
    if (str_eq(known_keys[i], key)) {
      return true;
    }
  }
  return false;
}
