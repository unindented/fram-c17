#include "formats/toml.h"

#include <stdint.h>
#include <string.h>

#include "core/error.h"
#include "core/text.h"

/**
 * @brief Reports whether a key matches one of a list of names.
 *
 * @param key             Key bytes, not necessarily `NUL`-terminated. Must not be `NULL`.
 * @param key_len         Length of `key` in bytes.
 * @param known_keys      Names to match against. May be `NULL` only when `known_key_count` is 0.
 * @param known_key_count Number of names in `known_keys`.
 * @return `true` when `key` equals one of `known_keys`, otherwise `false`.
 */
static bool is_known_key(const char* key,
                         size_t key_len,
                         const char* const* known_keys,
                         size_t known_key_count) __attribute__((nonnull(1)));

bool toml_datum_is_text(toml_datum_t value) {
  // tomlc17 terminates the string but excludes that terminator from `len`, so scanning exactly
  // `len` bytes finds only a `NUL` that the parser decoded from an escape.
  return value.type == TOML_STRING &&
         text_is_nul_free((const unsigned char*)value.u.str.ptr, (size_t)value.u.str.len);
}

int toml_require_known_keys(toml_datum_t table,
                            const char* const* known_keys,
                            size_t known_key_count,
                            const char* key_kind,
                            const char* key_prefix,
                            char* err,
                            size_t err_len) {
  for (int32_t i = 0; i < table.u.tab.size; i++) {
    const char* key = table.u.tab.key[i];
    const int key_len = table.u.tab.len[i];
    if (is_known_key(key, (size_t)key_len, known_keys, known_key_count)) {
      continue;
    }
    return error_report(err, err_len, "unknown %s key '%s%.*s'", key_kind, key_prefix, key_len,
                        key);
  }
  return 0;
}

static bool is_known_key(const char* key,
                         size_t key_len,
                         const char* const* known_keys,
                         size_t known_key_count) {
  for (size_t i = 0; i < known_key_count; i++) {
    if (strlen(known_keys[i]) == key_len && memcmp(known_keys[i], key, key_len) == 0) {
      return true;
    }
  }
  return false;
}
