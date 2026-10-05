#ifndef FRAM_TEMPLATE_H
#define FRAM_TEMPLATE_H

#include <stddef.h>

struct Album;
struct GalleryConfig;

/**
 * Borrowed data visible to templates during one render call. Every pointer is `const`, and that is
 * a thread-safety invariant. During the page phase every worker reaches other albums through
 * `sub_albums` and `parent`, and the `const` stops one job allocating into another album's arena or
 * the gallery configuration's. Do not relax it to add a mutable field.
 */
struct TemplateContext {
  /** Gallery metadata and configured paths. Must not be `NULL`. */
  const struct GalleryConfig* gallery_config;

  /** Album whose page-relative URL prefix applies to this render. Must not be `NULL`. */
  const struct Album* album_current;
};

/**
 * @brief Renders a template file into HTML using the given context.
 *
 * Loads `template_name` from `templates_dir`, renders it with mustache4c against `context`, and
 * transfers ownership of the result to the caller. The template name must be a safe relative path.
 * All scalar interpolation is HTML-escaped. Triple-brace interpolation is rejected so templates
 * cannot bypass that boundary.
 *
 * Templates are trusted input. The render limits terminate a partial that includes itself, bound
 * the output buffer, and bound the compiled-partial cache. They do not bound the render's total
 * memory: a template's own nested sections allocate in proportion to the iterations they ask for.
 *
 * @param templates_dir Template directory root. Must not be `NULL`.
 * @param template_name Safe relative template name within `templates_dir`. Must not be `NULL`.
 * @param context       Borrowed data visible to the template during this render. Must not be
 *                      `NULL`.
 * @param html_len_out  Receives the length of the returned HTML in bytes, excluding its terminator,
 *                      on success. May be `NULL` when the caller does not need it.
 * @param err           Buffer receiving a diagnostic that names the specific failure: an unsafe
 *                      name, an unreadable template or partial, a triple-brace interpolation, or an
 *                      exceeded limit. May be `NULL` only when `err_len` is 0.
 * @param err_len       Size of `err` in bytes.
 * @return Terminated HTML the caller must `free`, or `NULL` on failure, with a diagnostic in `err`.
 */
char* template_render_file(const char* templates_dir,
                           const char* template_name,
                           const struct TemplateContext* context,
                           size_t* html_len_out,
                           char* err,
                           size_t err_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Renders a template file into HTML under a caller-chosen output bound.
 *
 * Behaves as `template_render_file`, which calls it with the production bound. Reaching a byte
 * bound costs that many bytes, so this lets a test assert the output-limit diagnostic cheaply.
 *
 * @param templates_dir  Template directory root. Must not be `NULL`.
 * @param template_name  Safe relative template name within `templates_dir`. Must not be `NULL`.
 * @param context        Borrowed data visible to the template during this render. Must not be
 *                       `NULL`.
 * @param output_len_max Largest rendered output accepted, in bytes. A render whose accumulated
 *                       output passes it fails with a diagnostic naming it.
 * @param html_len_out   Receives the length of the returned HTML, as for `template_render_file`.
 *                       May be `NULL`.
 * @param err            Buffer receiving a diagnostic, as for `template_render_file`. May be `NULL`
 *                       only when `err_len` is 0.
 * @param err_len        Size of `err` in bytes.
 * @return Terminated HTML the caller must `free`, or `NULL` on failure, with a diagnostic in `err`.
 */
char* template_render_file_limited(const char* templates_dir,
                                   const char* template_name,
                                   const struct TemplateContext* context,
                                   size_t output_len_max,
                                   size_t* html_len_out,
                                   char* err,
                                   size_t err_len) __attribute__((nonnull(1, 2, 3)));

#endif
