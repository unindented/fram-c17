#include "domain/media_item.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core/ascii.h"
#include "shared/arena.h"

/**
 * @brief Reports whether a URL byte may be emitted literally.
 *
 * @param byte Byte to classify.
 * @return `true` for an RFC 3986 unreserved byte or `/`, otherwise `false`.
 */
static bool is_url_literal(unsigned char byte);

char* media_item_encode_url(const char* path, struct Arena* arena) {
  const size_t path_len = strlen(path);
  size_t encoded_len = 0;
  for (size_t i = 0; i < path_len; i++) {
    const size_t byte_len = is_url_literal((unsigned char)path[i]) ? 1 : 3;
    if (encoded_len > SIZE_MAX - byte_len) {
      return NULL;
    }
    encoded_len += byte_len;
  }
  if (encoded_len == SIZE_MAX) {
    return NULL;
  }

  char* encoded = arena_alloc(arena, encoded_len + 1);
  if (encoded == NULL) {
    return NULL;
  }
  static const char HEX[] = "0123456789ABCDEF";
  size_t pos = 0;
  for (size_t i = 0; i < path_len; i++) {
    const unsigned char byte = (unsigned char)path[i];
    if (is_url_literal(byte)) {
      encoded[pos++] = (char)byte;
    } else {
      encoded[pos++] = '%';
      encoded[pos++] = HEX[byte >> 4];
      encoded[pos++] = HEX[byte & 0x0F];
    }
  }
  encoded[pos] = '\0';
  return encoded;
}

bool media_item_has_extension(const char* path, const char* extension) {
  const size_t path_len = strlen(path);
  const size_t extension_len = strlen(extension);
  if (path_len < extension_len) {
    return false;
  }
  for (size_t i = 0; i < extension_len; i++) {
    if (ascii_to_lower((unsigned char)path[path_len - extension_len + i]) !=
        ascii_to_lower((unsigned char)extension[i])) {
      return false;
    }
  }
  return true;
}

bool media_item_is_video_path(const char* path) {
  return media_item_has_extension(path, ".mp4");
}

static bool is_url_literal(unsigned char byte) {
  return ascii_is_alphanumeric(byte) || byte == '-' || byte == '.' || byte == '_' || byte == '~' ||
         byte == '/';
}
