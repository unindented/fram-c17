#ifndef FRAM_HTML_H
#define FRAM_HTML_H

#include "shared/str.h"

struct Buf;

/**
 * @brief Appends `text` to `buffer`, HTML-escaped.
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
 * @param buffer Destination buffer to append to. Must not be `NULL`.
 * @param text   Source bytes to escape and append.
 */
void html_escape_append(struct Buf* buffer, struct Str text) __attribute__((nonnull(1)));

#endif
