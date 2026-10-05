#include "build/template.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "core/fram_version.h"
#include "core/path.h"
#include "core/text.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/media_item.h"
#include "formats/html.h"
#include "mustache.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"

// Bounds on one template render. There are three independent failure modes, so three limits, each
// enforced at the one place that can observe it. Each limit names what it bounds: the expansion
// count terminates partial recursion, the output length bounds the output buffer, and the distinct
// partial count bounds the compiled-partial cache. All three bound a single render, and renders run
// `worker_count`-wide, so several per-render costs can be live at once. That scaling is close to
// linear. On an 8-album runaway build at three names per expansion, peak resident memory rose from
// 138 MB with one worker to 697 MB with eight. Read the per-render figures below as costs that
// `worker_count` concurrent runaway renders multiply.
//
// None of the three bounds the render's total memory. Templates are trusted input, like the config.
// `node_alloc` puts a node in the per-render arena for every name resolution, and a section nested
// inside another resolves its names once per iteration of every enclosing section without touching
// the expansion counter. A template's own nested iteration therefore uses memory proportional to
// the iterations it asks for. Over 20 media items, `{{#is_video}}{{/is_video}}` inside five nested
// `{{#media}}` renders an empty page at 192 MB peak resident memory, and inside six at 3.8 GB.
//
// Termination: mustache4c expands partials iteratively through its own stack and exposes no nesting
// depth, but it calls the partial resolver once per expansion, cache hits included. Expansions are
// what can be counted, and counting them stops a partial that includes itself. A legitimate render
// costs one expansion per partial reference per media item, so an album page over 10,000 media
// items referencing ten partials each costs 100,000. The bound sits three times above that.
//
// The margin is three rather than ten because of what an expansion costs. mustache4c's own stack is
// 24 bytes per expansion: three `uintptr_t` pushes, at `mustache.c:1119`, heap-backed rather than
// thread stack. That is the smaller term. The larger one is this file's nodes, which the arena
// keeps until the render ends, and nothing bounds names-per-expansion. A template referencing nine
// names inside a self-including partial resolves nine nodes per expansion, so a runaway render's
// memory is `expansions × names`, not `expansions`. The bound has to be set against the product.
//
// These figures are peak resident memory of a whole `fram build` measured at this bound, one album,
// one worker, release build: 17 MB for a bare `{{>loop}}`, 86 MB at three names, 225 MB at nine,
// scaling linearly in names from there. A URL or number name costs more than a plain text one,
// because `node_scalar_url` and `node_scalar_size` also copy the formatted value into the arena.
// The nodes cannot be shared or interned to avoid this. See `node_alloc`, which states the vendored
// contract requiring pointer-unique nodes. Lowering the count further would start to reject the
// legitimate case above. A section iterating inside the recursive partial multiplies each
// expansion's cost by its iteration count, which no limit here bounds.
//
// Output: escaping expands one byte into as much as a six-byte entity, so the check bounds
// accumulated output after each append rather than from the incoming chunk length. This bounds the
// output buffer, not the render's other allocations. It is the only one of the three a legitimate
// gallery can reach, so the bound targets that case rather than a runaway one. An album page costs
// `media_count * item_markup`, and the fixture template's markup, dominated by each item's link and
// its derivative attributes, is ~1.3 KB per item. For 10,000 items that is 13 MB, so the bound sits
// about 20 times above the largest album this tool is meant for. That leaves room for a heavier
// template: one at ~10 KB per item still fits. The growable buffer doubles, so a render that
// reaches the bound allocates near three times it before failing. Resident memory stays lower,
// because the doubled capacity is never written. One that tripped the bound peaked at 280 MB.
// Reaching it costs that many bytes, so `template_render_file_limited` takes the bound as a
// parameter, and the test that asserts its diagnostic passes a smaller one.
//
// Capacity: the compiled-partial cache is a fixed array, so its length bounds the distinct partial
// count and with it the cache. This one limits real templates, not runaway ones. A cycle cannot
// inflate the distinct-name count, because the cache returns hits.
enum { RENDER_EXPANSION_COUNT_MAX = 300000 };
enum { RENDER_OUTPUT_LEN_MAX = 256 * 1024 * 1024 };
enum { RENDER_PARTIAL_COUNT_MAX = 64 };

_Static_assert((size_t)RENDER_PARTIAL_COUNT_MAX <= (size_t)RENDER_EXPANSION_COUNT_MAX,
               "a distinct partial costs at least one expansion, so its limit must be reachable");

/**
 * Largest template or partial file read, in bytes.
 *
 * A render reads its template and compiles up to `RENDER_PARTIAL_COUNT_MAX` distinct partials, and
 * the compiled form keeps a copy of every literal run, so 4 MiB each bounds one render's compiled
 * sources at 260 MiB, the same order as `RENDER_OUTPUT_LEN_MAX`. A hand-written template with
 * inline styles, scripts and SVG is tens of kilobytes, so this sits two orders of magnitude above
 * one. The read checks it before loading the file, so an oversize template is never resident.
 */
enum { TEMPLATE_FILE_LEN_MAX = 4 * 1024 * 1024 };

/**
 * Size of the stack buffer `node_get_partial_path` formats `partials/<name>.html` into. This is a
 * buffer bound on one path rather than a limit on the render's behavior.
 *
 * 256 leaves 241 bytes for the name, after `partials/` (9), `.html` (5) and the terminator. That is
 * far above any real partial name and deliberately below the 250 that `FILENAME_LEN_MAX` would
 * allow for `<name>.html`. This buffer, not the filesystem layer, is the first limit an absurd name
 * meets. The diagnostic the user sees names the partial instead of reporting an opaque read
 * failure.
 */
enum { PARTIAL_PATH_SIZE = 256 };

_Static_assert(PARTIAL_PATH_SIZE > sizeof("partials/") - 1 + sizeof(".html") - 1,
               "partial path buffer must hold the 'partials/' prefix and '.html' suffix");

/** Size of the stack buffer `node_scalar_size` formats one decimal `size_t` into. */
enum { SCALAR_NUMBER_SIZE = 32 };

/** Kind of a node in the immutable data tree walked by `mustache_process`. */
enum NodeKind {
  /** Top-level lookup context: `gallery.*`, the page's lists, and the current album's fields. */
  NODE_ROOT,

  /** Gallery metadata subtree resolving `gallery.*` names. */
  NODE_GALLERY,

  /** One album, resolving bare album field names. */
  NODE_ALBUM,

  /** The rendered album's `sub_albums`, iterated by index. */
  NODE_ALBUM_LIST,

  /** One media item, resolving bare media field names and derivative names. */
  NODE_MEDIA,

  /** One derivative of a media item, resolving its URL and presented dimensions. */
  NODE_DERIVATIVE,

  /** The rendered album's `media`, iterated by index. */
  NODE_MEDIA_LIST,

  /** The rendered album's ancestors from the root down to itself, iterated by index. */
  NODE_BREADCRUMB_LIST,

  /** A terminal string value. */
  NODE_SCALAR,
};

/** A node in the data tree. Nodes are arena-owned and live for one render. */
struct Node {
  /** Selects which of the fields below carry meaning for this node. */
  enum NodeKind kind;

  /** Album a `NODE_ALBUM` presents. */
  const struct Album* album;

  /** Backing media item for `NODE_MEDIA`. */
  const struct MediaItem* media;

  /** Backing derivative for `NODE_DERIVATIVE`. */
  const struct MediaDerivative* derivative;

  /**
   * Borrowed text for `NODE_SCALAR`. Never `NULL` and never empty, because `node_scalar` maps both
   * to an absent node, so `node_dump` may call `strlen` on it unguarded.
   */
  const char* scalar;
};

/** State threaded through the Mustache parser and data-provider callbacks. */
struct ProviderData {
  /** Template directory root used to locate partials. */
  const char* templates_dir;

  /** Name of the template being rendered, used in diagnostics. */
  const char* template_name;

  /**
   * Relative path of the template or partial currently being compiled, which `record_parse_error`
   * names as the file a syntax error is inside. Distinct from `template_name`, because a partial
   * compiles while mustache4c renders the outer template. This is a path rather than a bare name,
   * so the `(in '<file>')` clause denotes a file in both cases.
   */
  const char* template_name_compiling;

  /** Borrowed values visible to template variables. */
  const struct TemplateContext* context;

  /** Arena owning the nodes, partial bookkeeping, and formatted scalars of this render. */
  struct Arena* arena;

  /** Root node returned to `mustache_process`. */
  struct Node* node_root;

  /** Compiled partials, released after processing. */
  MUSTACHE_TEMPLATE* partials[RENDER_PARTIAL_COUNT_MAX];

  /** Partial names parallel to `partials`, used as a compile cache. */
  const char* partial_names[RENDER_PARTIAL_COUNT_MAX];

  /** Number of populated entries in `partials`/`partial_names`. */
  size_t partial_count;

  /** Partial expansions resolved so far, bounded by `RENDER_EXPANSION_COUNT_MAX`. */
  size_t expansion_count;

  /** Destination for the render's first failure diagnostic. May be `NULL` when `err_len` is 0. */
  char* err;

  /** Size of `err` in bytes. */
  size_t err_len;

  /**
   * `render_fail` sets this on the first failure in any callback, and
   * `template_render_file_limited` checks it once processing returns. That failure is an exceeded
   * limit, an unescaped interpolation, an unsafe partial name, an unreadable or uncompilable
   * partial, or an allocation failure.
   */
  bool has_failed;
};

/**
 * Destination for one render's output, passed to mustache4c as its renderer data. Pairs the buffer
 * and its bound with the provider state so an output callback that passes the bound can record the
 * diagnostic itself.
 */
struct RenderOutput {
  /** Buffer accumulating the rendered bytes. */
  struct StringBuffer* buffer;

  /** Largest accumulated length `buffer` may reach, in bytes. */
  size_t output_len_max;

  /** Provider state for this render, used to report a failed or oversize append. */
  struct ProviderData* provider_data;
};

/**
 * @brief Appends output text verbatim to the render buffer.
 *
 * This is installed in `renderer` as mustache4c's unescaped-output callback. `node_dump` refuses to
 * write a value through it, so only literal template text reaches this callback.
 *
 * @param output        Rendered bytes to append.
 * @param output_len    Number of bytes in `output`.
 * @param renderer_data Pointer to the destination `struct RenderOutput`.
 * @return `0` on success, or `-1` on allocation failure or an exceeded output limit.
 */
static int out_verbatim(const char* output, size_t output_len, void* renderer_data);

/**
 * @brief Appends output text HTML-escaped to the render buffer.
 *
 * This is installed in `renderer` as mustache4c's escaped-output callback, and is the only callback
 * a template value is written through.
 *
 * @param output        Rendered bytes to append.
 * @param output_len    Number of bytes in `output`.
 * @param renderer_data Pointer to the destination `struct RenderOutput`.
 * @return `0` on success, or `-1` on allocation failure or an exceeded output limit.
 */
static int out_escaped(const char* output, size_t output_len, void* renderer_data);

/**
 * @brief Fails the render when its accumulated output has passed its `output_len_max`.
 *
 * This runs after each append rather than before. Escaping expands a byte into as much as a
 * six-byte entity, so the incoming chunk length alone does not bound the growth. The overshoot is
 * one append, and an append is not necessarily small. `node_dump` hands `out_fn` a whole scalar in
 * a single call, and mustache4c hands `out_verbatim` each literal run of a template or partial
 * whole, so the largest append is the longest literal run or escaped value. A non-zero return
 * aborts `mustache_process`.
 *
 * @param render_output Render destination whose buffer length is checked. Must not be `NULL`.
 * @return `0` when the render may continue, or `-1` once the limit is passed.
 */
static int check_output_size(const struct RenderOutput* render_output) __attribute__((nonnull(1)));

/**
 * @brief Writes a node's terminal text through `out_fn`, rejecting unescaped interpolation.
 *
 * This is installed in `provider` as the value-rendering callback. Only scalar nodes produce text,
 * and other node kinds write nothing. A value the renderer asked to write unescaped fails the
 * render instead, so no template can put unescaped text in the page.
 *
 * @param node_ptr          Node to dump, as a `struct Node*`.
 * @param out_fn            Output callback supplied by the renderer.
 * @param renderer_data     Renderer state forwarded to `out_fn`.
 * @param provider_data_ptr Pointer to the render's `struct ProviderData`, which receives the
 *                          diagnostic for an unescaped interpolation.
 * @return The result of `out_fn`, `0` for a non-scalar node, or `-1` for unescaped interpolation.
 */
static int node_dump(void* node_ptr,
                     int (*out_fn)(const char*, size_t, void*),
                     void* renderer_data,
                     void* provider_data_ptr);

/**
 * @brief Returns the top-level lookup context node.
 *
 * This is installed in `provider` as the root-resolution callback.
 *
 * @param provider_data_ptr Pointer to the render's `struct ProviderData`.
 * @return The root node for name and index resolution.
 */
static void* node_get_root(void* provider_data_ptr);

/**
 * @brief Resolves a named child of a node.
 *
 * This is installed in `provider` as the by-name lookup callback. At the root a name selects the
 * `gallery.*` subtree, the rendered `album`, the page's `sub_albums`, `media`, or `breadcrumbs`
 * list, the `generator` string that names this tool and its version, or otherwise a field of the
 * album being rendered, such as `path_to_root`.
 *
 * @param node_ptr          Parent node, as a `struct Node*`.
 * @param name              Requested child name. Not `NUL`-terminated.
 * @param name_len          Length of `name` in bytes.
 * @param provider_data_ptr Pointer to the render's `struct ProviderData`.
 * @return The child node, or `NULL` when the name is not present.
 */
static void* node_get_child_by_name(void* node_ptr,
                                    const char* name,
                                    size_t name_len,
                                    void* provider_data_ptr);

/**
 * @brief Resolves an indexed child of a node.
 *
 * This is installed in `provider` as the by-index lookup callback. The sub-album, media, and
 * breadcrumb lists iterate the album being rendered. A non-list value is iterable once at index 0.
 *
 * @param node_ptr          Parent node, as a `struct Node*`.
 * @param index             Zero-based child index.
 * @param provider_data_ptr Pointer to the render's `struct ProviderData`.
 * @return The child node, or `NULL` when the index is out of range.
 */
static void* node_get_child_by_index(void* node_ptr, unsigned index, void* provider_data_ptr);

/**
 * @brief Loads, compiles, and caches a `partials/<name>.html` template.
 *
 * This is installed in `provider` as the partial-resolution callback, which mustache4c calls once
 * per expansion. Flags the render as failed when expansions pass the bound, the name is unsafe, the
 * cache is full, or the partial cannot be loaded or compiled.
 *
 * @param name              Partial name. Not `NUL`-terminated.
 * @param name_len          Length of `name` in bytes.
 * @param provider_data_ptr Pointer to the render's `struct ProviderData`.
 * @return The compiled partial template, or `NULL` on failure.
 */
static MUSTACHE_TEMPLATE* node_get_partial(const char* name,
                                           size_t name_len,
                                           void* provider_data_ptr);

/**
 * @brief Returns the already-compiled partial registered under `name`, if any.
 *
 * This matches on the length-delimited name, so the lookup runs before copying the name into the
 * arena. A partial referenced inside a loop resolves once per iteration, and copying first would
 * leak an arena allocation per occurrence.
 *
 * @param provider_data Render state holding the compile cache. Must not be `NULL`.
 * @param name          Partial name. Not `NUL`-terminated. Must not be `NULL`.
 * @param name_len      Length of `name` in bytes.
 * @return The cached compiled partial, or `NULL` when the name is not cached.
 */
static MUSTACHE_TEMPLATE* node_get_partial_cached(const struct ProviderData* provider_data,
                                                  const char* name,
                                                  size_t name_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Loads and compiles `partials/<name>.html`, reporting the reason it could not.
 *
 * Releases the read buffer on both the success and failure paths. Does not register the result in
 * the compile cache. The caller owns that bookkeeping.
 *
 * @param provider_data Render state supplying `templates_dir` and the arena, and receiving a
 *                      diagnostic on failure. Must not be `NULL`.
 * @param name          Terminated partial name, already validated as a safe identifier. Must not be
 *                      `NULL`.
 * @return The compiled partial template, or `NULL` on a path, read, NUL-byte, or compile failure.
 */
static MUSTACHE_TEMPLATE* node_get_partial_compile(struct ProviderData* provider_data,
                                                   const char* name) __attribute__((nonnull(1, 2)));

/**
 * @brief Builds the `partials/<name>.html` template path rooted under the render's `templates_dir`.
 *
 * Reports its own failure, because the two causes are distinct to the user. A name too long for
 * `PARTIAL_PATH_SIZE` names that limit, while a failed join is an allocation failure.
 *
 * @param provider_data     Render state supplying `templates_dir` and the arena, and receiving a
 *                          diagnostic on failure. Must not be `NULL`.
 * @param name              Terminated partial name, already validated as a safe identifier. Must
 *                          not be `NULL`.
 * @param relative_path_out Receives the `partials/<name>.html` form, arena-owned, on success only.
 *                          Handed back rather than rebuilt by the caller because that is the form
 *                          `record_parse_error` names. Must not be `NULL`.
 * @return The terminated joined path owned by the render arena, or `NULL` on an oversize name or
 *         allocation failure.
 */
static char* node_get_partial_path(struct ProviderData* provider_data,
                                   const char* name,
                                   const char** relative_path_out)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Allocates an arena-owned node of the given kind for the current render.
 *
 * Zeroes every field other than `kind`, so a caller sets only the fields its kind carries. Flags
 * the render as failed on allocation failure.
 *
 * @param provider_data Render provider state owning the node arena. Must not be `NULL`.
 * @param kind          Node kind to assign.
 * @return The new node, or `NULL` on allocation failure.
 */
static struct Node* node_alloc(struct ProviderData* provider_data, enum NodeKind kind)
    __attribute__((nonnull(1)));

/**
 * @brief Returns a scalar node wrapping `value`, or `NULL` when `value` is absent or empty.
 *
 * @param provider_data Render provider state owning the node arena. Must not be `NULL`.
 * @param value         Terminated string to wrap, borrowed for the rest of the render, or `NULL`.
 * @return A scalar node, or `NULL` when `value` is `NULL` or empty, or on allocation failure.
 */
static struct Node* node_scalar(struct ProviderData* provider_data, const char* value)
    __attribute__((nonnull(1)));

/**
 * @brief Returns a scalar node holding `value` in decimal.
 *
 * Copies the formatted digits into the render arena, because a scalar node borrows its text for the
 * rest of the render.
 *
 * @param provider_data Render provider state owning the node arena. Must not be `NULL`.
 * @param value         Number to format.
 * @return A scalar node, or `NULL` on a formatting or allocation failure.
 */
static struct Node* node_scalar_size(struct ProviderData* provider_data, size_t value)
    __attribute__((nonnull(1)));

/**
 * @brief Returns a scalar node holding an output-root-relative URL rebased onto the current page.
 *
 * Joins the URL behind the rendered album's `path_to_root`, so a link resolves from the page's own
 * directory rather than only from the output root.
 *
 * @param provider_data     Render provider state supplying the rendered album and the node arena.
 *                          Must not be `NULL`.
 * @param root_relative_url Percent-encoded output-root-relative URL. Must not be `NULL`.
 * @return A scalar node, or `NULL` on allocation failure.
 */
static struct Node* node_scalar_url(struct ProviderData* provider_data,
                                    const char* root_relative_url) __attribute__((nonnull(1)));

/**
 * @brief Resolves a `gallery.*` name against the render's gallery configuration.
 *
 * @param provider_data Render provider state holding the gallery configuration. Must not be `NULL`.
 * @param name          Requested field name. Not `NUL`-terminated. Must not be `NULL`.
 * @param name_len      Length of `name` in bytes.
 * @return The resolved node, or `NULL` when the name is unmatched or its value is unset.
 */
static struct Node* resolve_gallery(struct ProviderData* provider_data,
                                    const char* name,
                                    size_t name_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Resolves a bare field name against an album.
 *
 * `has_media` and `has_sub_albums` resolve to a truthy scalar only when the album holds that
 * content, so a section over either renders exactly when it is non-empty. A URL resolves already
 * rebased onto the page being rendered.
 *
 * @param provider_data Render provider state. Must not be `NULL`.
 * @param album         Album to resolve against. Must not be `NULL`.
 * @param name          Requested field name. Not `NUL`-terminated. Must not be `NULL`.
 * @param name_len      Length of `name` in bytes.
 * @return The resolved node, or `NULL` when the name is unmatched or the field is absent.
 */
static struct Node* resolve_album(struct ProviderData* provider_data,
                                  const struct Album* album,
                                  const char* name,
                                  size_t name_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Resolves a bare field name against a media item, including its derivative names.
 *
 * A name matching no media field is matched against the item's configured derivative names, so
 * `{{m.url}}` reaches the derivative named `m`. `is_image` and `is_video` resolve to a truthy
 * scalar only for that source kind, so a section over either renders exactly for it.
 *
 * @param provider_data Render provider state. Must not be `NULL`.
 * @param media         Media item to resolve against. Must not be `NULL`.
 * @param name          Requested field name. Not `NUL`-terminated. Must not be `NULL`.
 * @param name_len      Length of `name` in bytes.
 * @return The resolved node, or `NULL` when the name is unmatched or the field is absent.
 */
static struct Node* resolve_media(struct ProviderData* provider_data,
                                  const struct MediaItem* media,
                                  const char* name,
                                  size_t name_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Resolves a bare field name against one derivative of a media item.
 *
 * @param provider_data Render provider state. Must not be `NULL`.
 * @param derivative    Derivative to resolve against. Must not be `NULL`.
 * @param name          Requested field name. Not `NUL`-terminated. Must not be `NULL`.
 * @param name_len      Length of `name` in bytes.
 * @return The resolved node, or `NULL` when the name is unmatched.
 */
static struct Node* resolve_derivative(struct ProviderData* provider_data,
                                       const struct MediaDerivative* derivative,
                                       const char* name,
                                       size_t name_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Reports whether a length-delimited name equals a terminated literal.
 *
 * @param name     Name bytes. Not `NUL`-terminated. Must not be `NULL`.
 * @param name_len Length of `name` in bytes.
 * @param expected Terminated literal to compare against. Must not be `NULL`.
 * @return `true` when the two are byte-for-byte equal, `false` otherwise.
 */
static bool is_name_equal(const char* name, size_t name_len, const char* expected)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Records the render's first failure diagnostic and marks the render as failed.
 *
 * Only the first message is kept: it names the root cause, and later callbacks typically fail as a
 * consequence of it. Writes nothing when the caller asked for no diagnostic.
 *
 * @param provider_data Render provider state holding the diagnostic buffer and failure flag. Must
 *                      not be `NULL`.
 * @param fmt           `printf`-style format string. Must not be `NULL`.
 * @param ...           Arguments for `fmt`.
 */
static void render_fail(struct ProviderData* provider_data, const char* fmt, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Records a template syntax error, naming the reason and where it is.
 *
 * Installed in `parser` as mustache4c's parse-error callback. Without it the library substitutes a
 * no-op and `mustache_compile` reports only that it failed, so the line and column are lost and the
 * user has to find a malformed tag by eye.
 *
 * mustache4c can report several errors for one template, such as the secondary
 * `MUSTACHE_ERR_SECTIONOPENERHERE` note locating the opener of an unclosed section. `render_fail`
 * keeps the first message, which is the primary error.
 *
 * @param err_code    mustache4c `MUSTACHE_ERR_*` code. Unused, since `msg` already names the cause.
 * @param msg         Library message for `err_code`. Must not be `NULL`.
 * @param line        One-based line of the offending tag.
 * @param column      One-based column of the offending tag.
 * @param parser_data Pointer to the render's `struct ProviderData`. Must not be `NULL`.
 */
static void record_parse_error(int err_code,
                               const char* msg,
                               unsigned line,
                               unsigned column,
                               void* parser_data) __attribute__((nonnull(2, 5)));

/**
 * Renderer dispatch table. These tables are `const` so they stay in read-only data. Renders run
 * concurrently in pool jobs and `src/` deliberately holds zero writable globals.
 * `gallery_config.c`'s default template array has this same shape, and lands in read-only data only
 * because its elements are `const` too.
 */
static const MUSTACHE_RENDERER renderer = {out_verbatim, out_escaped};

/** Data-provider dispatch table. */
static const MUSTACHE_DATAPROVIDER provider = {node_dump, node_get_root, node_get_child_by_name,
                                               node_get_child_by_index, node_get_partial};

/** Parser dispatch table. */
static const MUSTACHE_PARSER parser = {record_parse_error};

char* template_render_file(const char* templates_dir,
                           const char* template_name,
                           const struct TemplateContext* context,
                           size_t* html_len_out,
                           char* err,
                           size_t err_len) {
  return template_render_file_limited(templates_dir, template_name, context,
                                      (size_t)RENDER_OUTPUT_LEN_MAX, html_len_out, err, err_len);
}

char* template_render_file_limited(const char* templates_dir,
                                   const char* template_name,
                                   const struct TemplateContext* context,
                                   size_t output_len_max,
                                   size_t* html_len_out,
                                   char* err,
                                   size_t err_len) {
  if (!path_is_safe_relative(template_name)) {
    (void)error_report(err, err_len, "template must be a safe relative template name: '%s'",
                       template_name);
    return NULL;
  }

  // Every allocation this render makes goes into `scratch`, which this call creates and destroys.
  // Renders run concurrently in pool jobs, so the per-call arena keeps two renders out of each
  // other's memory. Nothing in this file may allocate into an arena reached through `context`.
  struct Arena scratch;
  arena_init(&scratch);
  struct StringBuffer buffer;
  string_buffer_init(&buffer);
  struct Node node_root = {.kind = NODE_ROOT};
  // Because this literal names only the non-zero values, initialization cannot accidentally omit a
  // new `ProviderData` field. `partial_count`, `expansion_count`, and `has_failed` start at zero.
  // The code assigns `partials` and `partial_names` when a partial compiles. It assigns
  // `template_name_compiling` before both `mustache_compile` calls. Only those calls can reach
  // `record_parse_error`.
  struct ProviderData provider_data = {.templates_dir = templates_dir,
                                       .template_name = template_name,
                                       .context = context,
                                       .arena = &scratch,
                                       .node_root = &node_root,
                                       .err = err,
                                       .err_len = err_len};
  struct RenderOutput render_output = {
      .buffer = &buffer, .output_len_max = output_len_max, .provider_data = &provider_data};

  unsigned char* template_data = NULL;
  size_t template_len = 0;
  MUSTACHE_TEMPLATE* templ = NULL;
  char* rendered_html = NULL;
  size_t rendered_len = 0;

  char* template_path = path_join(templates_dir, template_name, &scratch);
  if (template_path == NULL) {
    (void)error_report(err, err_len, "out of memory building path for template '%s'",
                       template_name);
    goto cleanup;
  }
  char reason[FS_REASON_SIZE];
  if (fs_read_file(template_path, TEMPLATE_FILE_LEN_MAX, &template_data, &template_len, reason,
                   sizeof(reason)) != 0) {
    (void)error_report(err, err_len, "failed to read template: %s ('%s')", reason, template_path);
    goto cleanup;
  }
  if (!text_is_nul_free(template_data, template_len)) {
    (void)error_report(err, err_len,
                       "failed to read template: contains an embedded NUL byte ('%s')",
                       template_path);
    goto cleanup;
  }

  provider_data.template_name_compiling = template_name;
  templ = mustache_compile((const char*)template_data, template_len, &parser, &provider_data, 0);
  if (templ == NULL) {
    // `record_parse_error` has already named the reason and its line for a syntax error. Only fall
    // back to a generic message when the compile failed without reporting one.
    if (!provider_data.has_failed) {
      (void)error_report(err, err_len, "failed to compile template '%s'", template_name);
    }
    goto cleanup;
  }
  // The render needs both failure signals. `mustache_process` reports only a callback that returned
  // non-zero, and a `NULL` from `node_get_partial` or `node_get_child_by_name` means *absent* to
  // mustache4c rather than *failed*. It skips the partial and renders on. Without `has_failed` an
  // unreadable partial or a failed node allocation would yield a successful, silently incomplete
  // page. This line is also the boundary where mustache4c's zero/non-zero convention becomes this
  // project's `NULL`-on-failure producer contract.
  if (mustache_process(templ, &renderer, &render_output, &provider, &provider_data) != 0 ||
      provider_data.has_failed) {
    // A failing provider callback has already recorded the specific cause. Only fall back to a
    // generic message when the renderer itself failed.
    if (!provider_data.has_failed) {
      (void)error_report(err, err_len, "failed to render template '%s'", template_name);
    }
    goto cleanup;
  }
  // A template that rendered nothing still yields an allocated, terminated buffer to steal.
  rendered_len = buffer.len;
  rendered_html = string_buffer_steal(&buffer);
  if (rendered_html == NULL) {
    (void)error_report(err, err_len, "out of memory rendering template '%s'", template_name);
  } else if (html_len_out != NULL) {
    *html_len_out = rendered_len;
  }

cleanup:
  for (size_t i = 0; i < provider_data.partial_count; i++) {
    mustache_release(provider_data.partials[i]);
  }
  if (templ != NULL) {
    mustache_release(templ);
  }
  free(template_data);
  string_buffer_free(&buffer);
  arena_free(&scratch);
  return rendered_html;
}

static int out_verbatim(const char* output, size_t output_len, void* renderer_data) {
  struct RenderOutput* render_output = renderer_data;
  if (string_buffer_append_len(render_output->buffer, output, output_len) != 0) {
    render_fail(render_output->provider_data, "out of memory rendering template '%s'",
                render_output->provider_data->template_name);
    return -1;
  }
  return check_output_size(render_output);
}

static int out_escaped(const char* output, size_t output_len, void* renderer_data) {
  struct RenderOutput* render_output = renderer_data;
  if (html_escape_append_len(render_output->buffer, output, output_len) != 0) {
    render_fail(render_output->provider_data, "out of memory rendering template '%s'",
                render_output->provider_data->template_name);
    return -1;
  }
  return check_output_size(render_output);
}

static int check_output_size(const struct RenderOutput* render_output) {
  if (render_output->buffer->len <= render_output->output_len_max) {
    return 0;
  }
  render_fail(render_output->provider_data,
              "render exceeds max rendered output (%zu bytes) at %zu bytes (in '%s')",
              render_output->output_len_max, render_output->buffer->len,
              render_output->provider_data->template_name);
  return -1;
}

static int node_dump(void* node_ptr,
                     int (*out_fn)(const char*, size_t, void*),
                     void* renderer_data,
                     void* provider_data_ptr) {
  const struct Node* node = node_ptr;
  if (node->kind != NODE_SCALAR) {
    return 0;
  }
  // The callback identity is what distinguishes `{{{value}}}` from `{{value}}`. mustache4c passes
  // the renderer's unescaped-output callback for a triple-brace or ampersand tag and names the tag
  // kind nowhere else, so comparing against `out_escaped` is how this render refuses to emit a
  // value unescaped.
  if (out_fn != out_escaped) {
    struct ProviderData* provider_data = provider_data_ptr;
    render_fail(provider_data, "unescaped Mustache interpolation is not supported (in '%s')",
                provider_data->template_name);
    return -1;
  }
  return out_fn(node->scalar, strlen(node->scalar), renderer_data);
}

static void* node_get_root(void* provider_data_ptr) {
  return ((struct ProviderData*)provider_data_ptr)->node_root;
}

static void* node_get_child_by_name(void* node_ptr,
                                    const char* name,
                                    size_t name_len,
                                    void* provider_data_ptr) {
  struct ProviderData* provider_data = provider_data_ptr;
  const struct Node* node = node_ptr;
  switch (node->kind) {
    case NODE_ROOT:
      if (is_name_equal(name, name_len, "gallery")) {
        return node_alloc(provider_data, NODE_GALLERY);
      }
      if (is_name_equal(name, name_len, "album")) {
        struct Node* child = node_alloc(provider_data, NODE_ALBUM);
        if (child != NULL) {
          child->album = provider_data->context->album_current;
        }
        return child;
      }
      if (is_name_equal(name, name_len, "sub_albums")) {
        return node_alloc(provider_data, NODE_ALBUM_LIST);
      }
      if (is_name_equal(name, name_len, "media")) {
        return node_alloc(provider_data, NODE_MEDIA_LIST);
      }
      if (is_name_equal(name, name_len, "breadcrumbs")) {
        return node_alloc(provider_data, NODE_BREADCRUMB_LIST);
      }
      if (is_name_equal(name, name_len, "generator")) {
        return node_scalar(provider_data, fram_generator_string());
      }
      return resolve_album(provider_data, provider_data->context->album_current, name, name_len);
    case NODE_GALLERY:
      return resolve_gallery(provider_data, name, name_len);
    case NODE_ALBUM:
      return resolve_album(provider_data, node->album, name, name_len);
    case NODE_MEDIA:
      return resolve_media(provider_data, node->media, name, name_len);
    case NODE_DERIVATIVE:
      return resolve_derivative(provider_data, node->derivative, name, name_len);
    case NODE_ALBUM_LIST:
    case NODE_MEDIA_LIST:
    case NODE_BREADCRUMB_LIST:
    case NODE_SCALAR:
      return NULL;
  }
  // Unreachable: the switch covers every `enum NodeKind`, and omits `default:` so that `-Wswitch`
  // fails the build when a kind is added without a case here.
  abort();
}

static void* node_get_child_by_index(void* node_ptr, unsigned index, void* provider_data_ptr) {
  struct ProviderData* provider_data = provider_data_ptr;
  struct Node* node = node_ptr;
  const struct Album* current = provider_data->context->album_current;
  if (node->kind == NODE_ALBUM_LIST) {
    if (index >= current->sub_album_count) {
      return NULL;
    }
    struct Node* child = node_alloc(provider_data, NODE_ALBUM);
    if (child != NULL) {
      child->album = current->sub_albums[index];
    }
    return child;
  }
  if (node->kind == NODE_MEDIA_LIST) {
    if (index >= current->media_count) {
      return NULL;
    }
    struct Node* child = node_alloc(provider_data, NODE_MEDIA);
    if (child != NULL) {
      child->media = current->media[index];
    }
    return child;
  }
  if (node->kind == NODE_BREADCRUMB_LIST) {
    if (index > current->depth) {
      return NULL;
    }
    const struct Album* crumb = current;
    for (size_t remaining = current->depth - index; remaining > 0; remaining--) {
      crumb = crumb->parent;
    }
    struct Node* child = node_alloc(provider_data, NODE_ALBUM);
    if (child != NULL) {
      child->album = crumb;
    }
    return child;
  }
  // `vendor/mustache4c/mustache.h` requires this. Per the Mustache specification a single value is
  // iterable too, so this callback returns the node itself for index 0 and `NULL` for every other
  // index. Section truthiness uses the same call. mustache4c asks for child 0 to decide whether
  // `{{#x}}` enters and whether `{{^x}}` fires.
  return index == 0 ? node : NULL;
}

static MUSTACHE_TEMPLATE* node_get_partial(const char* name,
                                           size_t name_len,
                                           void* provider_data_ptr) {
  struct ProviderData* provider_data = provider_data_ptr;

  // The counter increments before the cache lookup, so the bound holds for a cycle whose partial is
  // already compiled. This is the render's only termination guard.
  provider_data->expansion_count++;
  if (provider_data->expansion_count > (size_t)RENDER_EXPANSION_COUNT_MAX) {
    render_fail(provider_data,
                "render exceeds max partial expansions (%d); check for a partial that includes "
                "itself (in '%s')",
                RENDER_EXPANSION_COUNT_MAX, provider_data->template_name);
    return NULL;
  }

  // The cache lookup precedes the arena copy below, so a repeated reference costs no allocation.
  MUSTACHE_TEMPLATE* cached = node_get_partial_cached(provider_data, name, name_len);
  if (cached != NULL) {
    return cached;
  }

  char* name_z = arena_strndup(provider_data->arena, name, name_len);
  if (name_z == NULL) {
    render_fail(provider_data, "out of memory resolving partial '%.*s'", (int)name_len, name);
    return NULL;
  }
  if (!text_is_safe_identifier(name_z)) {
    // This is the directory-traversal guard, so it cannot be dropped as redundant input validation.
    // The partial name becomes a filesystem path, so this check validates it here rather than
    // trusting it to mustache4c. `mustache_validate_tagname` happens to reject `..` and to accept
    // `/`, and neither rule is part of `mustache.h`'s contract, so `{{>a/b}}` does reach this
    // callback and only this check keeps a partial inside `partials/`. The name trails the reason
    // because `mustache.h` bounds a tag name nowhere, so a long `{{>...}}` would otherwise truncate
    // the reason away.
    render_fail(provider_data, "partial name must contain only letters, digits, '_' and '-': '%s'",
                name_z);
    return NULL;
  }
  if (provider_data->partial_count >= RENDER_PARTIAL_COUNT_MAX) {
    render_fail(provider_data,
                "render exceeds max distinct partials (%d) while resolving partial '%s' (in '%s')",
                RENDER_PARTIAL_COUNT_MAX, name_z, provider_data->template_name);
    return NULL;
  }

  MUSTACHE_TEMPLATE* templ = node_get_partial_compile(provider_data, name_z);
  if (templ == NULL) {
    return NULL;
  }
  provider_data->partials[provider_data->partial_count] = templ;
  provider_data->partial_names[provider_data->partial_count] = name_z;
  provider_data->partial_count++;
  return templ;
}

static MUSTACHE_TEMPLATE* node_get_partial_cached(const struct ProviderData* provider_data,
                                                  const char* name,
                                                  size_t name_len) {
  for (size_t i = 0; i < provider_data->partial_count; i++) {
    if (is_name_equal(name, name_len, provider_data->partial_names[i])) {
      return provider_data->partials[i];
    }
  }
  return NULL;
}

static MUSTACHE_TEMPLATE* node_get_partial_compile(struct ProviderData* provider_data,
                                                   const char* name) {
  const char* relative_path = NULL;
  char* partial_path = node_get_partial_path(provider_data, name, &relative_path);
  if (partial_path == NULL) {
    return NULL;
  }
  unsigned char* partial_data = NULL;
  size_t partial_len = 0;
  char reason[FS_REASON_SIZE];
  if (fs_read_file(partial_path, TEMPLATE_FILE_LEN_MAX, &partial_data, &partial_len, reason,
                   sizeof(reason)) != 0) {
    render_fail(provider_data, "failed to read partial: %s ('%s')", reason, partial_path);
    return NULL;
  }
  if (!text_is_nul_free(partial_data, partial_len)) {
    render_fail(provider_data, "failed to read partial: contains an embedded NUL byte ('%s')",
                partial_path);
    free(partial_data);
    return NULL;
  }

  // This uses the relative path, not the bare `name`. `record_parse_error` composes this into
  // `(in '<file>')`, which must denote a file.
  provider_data->template_name_compiling = relative_path;
  MUSTACHE_TEMPLATE* templ =
      mustache_compile((const char*)partial_data, partial_len, &parser, provider_data, 0);
  // Freeing this immediately is safe because `mustache_compile` copies every literal run and tag
  // name into the instruction buffer it returns. `mustache.h` never states that lifetime, so this
  // rests on reading `mustache.c`. A vendor that started borrowing `templ_data` would turn this
  // into a use-after-free for the rest of the render. The `free` also precedes the `templ == NULL`
  // check, so the failing path does not leak it.
  free(partial_data);
  if (templ == NULL) {
    // `record_parse_error` names a syntax error itself, and `render_fail` keeps the first message,
    // so this is the fallback for a compile that failed without reporting a reason.
    render_fail(provider_data, "failed to compile partial '%s'", name);
    return NULL;
  }
  return templ;
}

static char* node_get_partial_path(struct ProviderData* provider_data,
                                   const char* name,
                                   const char** relative_path_out) {
  char formatted[PARTIAL_PATH_SIZE];
  const int n = snprintf(formatted, sizeof(formatted), "partials/%s.html", name);
  if (n < 0 || (size_t)n >= sizeof(formatted)) {
    render_fail(provider_data,
                "partial path exceeds max partial path length (%zu bytes) at %d bytes: '%s'",
                sizeof(formatted) - 1, n, name);
    return NULL;
  }
  // This is copied into the arena because the caller holds it for the whole compile, while
  // `formatted` dies with this call.
  const char* relative_path = arena_strdup(provider_data->arena, formatted);
  char* partial_path = relative_path == NULL ? NULL
                                             : path_join(provider_data->templates_dir,
                                                         relative_path, provider_data->arena);
  if (partial_path == NULL) {
    render_fail(provider_data, "out of memory building path for partial '%s'", name);
    return NULL;
  }
  *relative_path_out = relative_path;
  return partial_path;
}

static struct Node* node_alloc(struct ProviderData* provider_data, enum NodeKind kind) {
  // This allocates a fresh node per resolution, never a shared singleton, even for the kinds that
  // carry no per-instance state. `vendor/mustache4c/mustache.h` requires each node of the hierarchy
  // to be uniquely identified by its pointer. Today's `mustache.c` only pushes and pops them, so
  // sharing would work, but that is an implementation detail a vendor bump can change without a
  // word. The allocation reads as an unnoticed inefficiency, so do not replace it with a shared
  // node.
  struct Node* node = arena_calloc(provider_data->arena, 1, sizeof(*node));
  if (node == NULL) {
    render_fail(provider_data, "out of memory building template render context");
    return NULL;
  }
  node->kind = kind;
  return node;
}

static struct Node* node_scalar(struct ProviderData* provider_data, const char* value) {
  // An empty value is absent, not present-and-empty, so `{{#field}}` skips it and `{{^field}}`
  // fires. mustache4c decides a section's truthiness by asking for child 0. A non-list node answers
  // with itself, so returning a node here for `""` would make a section over an unset field always
  // render. Interpolation is unchanged. `{{field}}` writes zero bytes whether the node is absent or
  // holds an empty string. `resolve_album` already applies this same rule to `has_media` and
  // `has_sub_albums`.
  if (value == NULL || *value == '\0') {
    return NULL;
  }
  struct Node* node = node_alloc(provider_data, NODE_SCALAR);
  if (node != NULL) {
    node->scalar = value;
  }
  return node;
}

static struct Node* node_scalar_size(struct ProviderData* provider_data, size_t value) {
  char number[SCALAR_NUMBER_SIZE];
  const int number_len = snprintf(number, sizeof(number), "%zu", value);
  if (number_len < 0 || (size_t)number_len >= sizeof(number)) {
    render_fail(provider_data, "failed to format template integer");
    return NULL;
  }
  char* copy = arena_strdup(provider_data->arena, number);
  if (copy == NULL) {
    render_fail(provider_data, "out of memory building template render context");
    return NULL;
  }
  return node_scalar(provider_data, copy);
}

static struct Node* node_scalar_url(struct ProviderData* provider_data,
                                    const char* root_relative_url) {
  char* relative = path_join(provider_data->context->album_current->path_to_root, root_relative_url,
                             provider_data->arena);
  if (relative == NULL) {
    render_fail(provider_data, "out of memory building page-relative URL");
    return NULL;
  }
  return node_scalar(provider_data, relative);
}

static struct Node* resolve_gallery(struct ProviderData* provider_data,
                                    const char* name,
                                    size_t name_len) {
  const struct GalleryConfig* gallery_config = provider_data->context->gallery_config;
  if (is_name_equal(name, name_len, "title")) {
    return node_scalar(provider_data, gallery_config->title);
  }
  if (is_name_equal(name, name_len, "author")) {
    return node_scalar(provider_data, gallery_config->author);
  }
  if (is_name_equal(name, name_len, "base_url")) {
    return node_scalar(provider_data, gallery_config->base_url);
  }
  return NULL;
}

static struct Node* resolve_album(struct ProviderData* provider_data,
                                  const struct Album* album,
                                  const char* name,
                                  size_t name_len) {
  if (is_name_equal(name, name_len, "title")) {
    return node_scalar(provider_data, album->title);
  }
  if (is_name_equal(name, name_len, "url")) {
    return node_scalar_url(provider_data, album->url_path);
  }
  if (is_name_equal(name, name_len, "path_to_root")) {
    return node_scalar(provider_data, album->path_to_root);
  }
  if (is_name_equal(name, name_len, "item_count")) {
    return node_scalar_size(provider_data, album->item_count_total);
  }
  if (is_name_equal(name, name_len, "cover")) {
    if (album->cover == NULL) {
      return NULL;
    }
    struct Node* cover = node_alloc(provider_data, NODE_MEDIA);
    if (cover != NULL) {
      cover->media = album->cover;
    }
    return cover;
  }
  if (is_name_equal(name, name_len, "has_media")) {
    return album->media_count == 0 ? NULL : node_scalar(provider_data, "true");
  }
  if (is_name_equal(name, name_len, "has_sub_albums")) {
    return album->sub_album_count == 0 ? NULL : node_scalar(provider_data, "true");
  }
  if (is_name_equal(name, name_len, "parent")) {
    if (album->parent == NULL) {
      return NULL;
    }
    struct Node* parent = node_alloc(provider_data, NODE_ALBUM);
    if (parent != NULL) {
      parent->album = album->parent;
    }
    return parent;
  }
  return NULL;
}

static struct Node* resolve_media(struct ProviderData* provider_data,
                                  const struct MediaItem* media,
                                  const char* name,
                                  size_t name_len) {
  if (is_name_equal(name, name_len, "title")) {
    return node_scalar(provider_data, media->title);
  }
  if (is_name_equal(name, name_len, "is_video")) {
    return media->kind == MEDIA_KIND_VIDEO ? node_scalar(provider_data, "true") : NULL;
  }
  if (is_name_equal(name, name_len, "is_image")) {
    return media->kind == MEDIA_KIND_IMAGE ? node_scalar(provider_data, "true") : NULL;
  }
  if (is_name_equal(name, name_len, "original_url")) {
    return node_scalar_url(provider_data, media->original_url);
  }
  if (is_name_equal(name, name_len, "duration")) {
    return node_scalar_size(provider_data, media->duration_ms);
  }
  for (size_t i = 0; i < media->derivative_count; i++) {
    const struct MediaDerivative* derivative = &media->derivatives[i];
    if (is_name_equal(name, name_len, derivative->name)) {
      struct Node* child = node_alloc(provider_data, NODE_DERIVATIVE);
      if (child != NULL) {
        child->derivative = derivative;
      }
      return child;
    }
  }
  return NULL;
}

static struct Node* resolve_derivative(struct ProviderData* provider_data,
                                       const struct MediaDerivative* derivative,
                                       const char* name,
                                       size_t name_len) {
  if (is_name_equal(name, name_len, "url")) {
    return node_scalar_url(provider_data, derivative->url);
  }
  if (is_name_equal(name, name_len, "width")) {
    return node_scalar_size(provider_data, derivative->width_px);
  }
  if (is_name_equal(name, name_len, "height")) {
    return node_scalar_size(provider_data, derivative->height_px);
  }
  return NULL;
}

static bool is_name_equal(const char* name, size_t name_len, const char* expected) {
  // The comparison must check the lengths first, and the `&&` short circuit guarantees it. `memcmp`
  // may read all `name_len` bytes however early the first difference falls, so comparing without
  // already knowing the lengths match reads past the end of `expected` whenever `name_len` is the
  // longer. That is undefined behavior, not merely a wrong answer.
  return strlen(expected) == name_len && memcmp(name, expected, name_len) == 0;
}

static void render_fail(struct ProviderData* provider_data, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (!provider_data->has_failed) {
    error_report_va(provider_data->err, provider_data->err_len, fmt, ap);
  }
  va_end(ap);
  provider_data->has_failed = true;
}

static void record_parse_error(int err_code,
                               const char* msg,
                               unsigned line,
                               unsigned column,
                               void* parser_data) {
  (void)err_code;
  struct ProviderData* provider_data = parser_data;
  render_fail(provider_data, "%s at line %u, column %u (in '%s')", msg, line, column,
              provider_data->template_name_compiling);
}
