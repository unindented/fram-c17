#ifndef FRAM_HTML_H
#define FRAM_HTML_H

#include <stddef.h>

struct StringBuffer;

/**
 * @brief Appends the first `text_len` bytes of `text` to `buffer`, HTML-escaped.
 *
 * Replaces `&`, `<`, `>`, `"`, and `'` with their character references and copies every other byte
 * verbatim.
 *
 * This is the project's escaping boundary against markup injection. Every `{{ ... }}` interpolation
 * in a template reaches it through `out_escaped`, including album and media names discovered from
 * the filesystem. The five bytes are sufficient for the two contexts that templates use:
 * - element text
 * - a quoted attribute value in HTML or XML, with either quote style
 *
 * They are not sufficient anywhere else. Do not place the result in:
 * - an unquoted attribute value (whitespace, `=` and a backtick pass through, so the value can end
 *   early and inject an attribute)
 * - a raw-text element such as `<script>` or `<style>` (character references are not decoded there,
 *   so escaping corrupts the text instead of protecting it)
 * - a URL-valued attribute such as `href` (a `javascript:` scheme survives escaping untouched)
 *
 * Escaping is also not XML validation. A control byte that XML 1.0 forbids passes through verbatim.
 *
 * @param buffer   Destination buffer to append to. Must not be `NULL`.
 * @param text     Source bytes. Must hold at least `text_len` bytes. May be `NULL` only when
 *                 `text_len` is 0, and must not point into `buffer->data`. It forwards the bytes to
 *                 `string_buffer_append_len`. A growth frees the region `text` points at before the
 *                 copy runs.
 * @param text_len Number of bytes to escape and append.
 * @return `0` on success, or `-1` on allocation failure. After failure, `buffer` contains only the
 *         text appended before the failed call. Do not reuse it for output. In contrast,
 *         `string_buffer_append_len` leaves its buffer unchanged after failure.
 */
int html_escape_append_len(struct StringBuffer* buffer, const char* text, size_t text_len)
    __attribute__((nonnull(1)));

#endif
