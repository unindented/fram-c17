#include "build/template.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/fram_version.h"
#include "core/path.h"
#include "core/text.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/media_item.h"
#include "formats/html.h"
#include "mustache.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

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
// because `node_scalar_url` and `node_scalar_number` also copy the formatted value into the arena.
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
// template: one at ~10 KB per item still fits. The output buffer grows in place in the caller's
// arena and doubles, so a render that reaches the bound reserves up to twice it before failing.
// Resident memory stays lower, because the doubled capacity is never written. Reaching the bound
// costs that many bytes, so `template_render_file_limited` takes the bound as a parameter, and the
// test that asserts its diagnostic passes a smaller one.
//
// Capacity: the compiled-partial cache is a fixed array, so its length bounds the distinct partial
// count and with it the cache. This one limits real templates, not runaway ones. A cycle cannot
// inflate the distinct-name count, because the cache returns hits.
//
// `RENDER_OUTPUT_LEN_MAX` and `TEMPLATE_FILE_LEN_MAX` are declared in `template.h`, because the
// build sizes its perms from the first and the tests read the second.
enum { RENDER_EXPANSION_COUNT_MAX = 300000 };
enum { RENDER_PARTIAL_COUNT_MAX = 64 };

_Static_assert((ptrdiff_t)RENDER_PARTIAL_COUNT_MAX <= (ptrdiff_t)RENDER_EXPANSION_COUNT_MAX,
               "a distinct partial costs at least one expansion, so its limit must be reachable");

/** Size of the stack buffer `node_scalar_number` formats one decimal `ptrdiff_t` into. */
enum { SCALAR_NUMBER_SIZE = 32 };

// A decimal `ptrdiff_t` at its widest, sign included, so a number formatted in `node_scalar_number`
// cannot truncate.
_Static_assert(SCALAR_NUMBER_SIZE > sizeof("-9223372036854775808") - 1,
               "scalar number buffer must hold the widest ptrdiff_t");

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
   * Borrowed text for `NODE_SCALAR`. Never empty, because `node_scalar` maps an empty value to an
   * absent node.
   */
  struct Str scalar;
};

/** State threaded through the Mustache parser and data-provider callbacks. */
struct ProviderData {
  /** Host the template and its partials are read from. */
  struct Os* os;

  /** Template directory root used to locate partials. */
  struct Str templates_dir;

  /** Name of the template being rendered, used in diagnostics. */
  struct Str template_name;

  /**
   * Relative path of the template or partial currently being compiled, which `record_parse_error`
   * names as the file a syntax error is inside. Distinct from `template_name`, because a partial
   * compiles while mustache4c renders the outer template. This is a path rather than a bare name,
   * so the `(in '<file>')` clause denotes a file in both cases.
   */
  struct Str template_name_compiling;

  /** Borrowed values visible to template variables. */
  const struct TemplateContext* context;

  /**
   * Arena owning the nodes, partial bookkeeping, and formatted scalars of this render. It points at
   * the render's by-value scratch, so all of it is released when the render returns. While a
   * partial compiles, it points at the copy of that scratch holding the partial's bytes.
   */
  struct Arena* arena;

  /** Root node returned to `mustache_process`. */
  struct Node* node_root;

  /**
   * Compiled top-level template, which `node_get_partial_holder` tells apart from the compiled
   * partials when it names the file holding a `{{>...}}` reference.
   */
  MUSTACHE_TEMPLATE* templ;

  /** Compiled partials, released after processing. */
  MUSTACHE_TEMPLATE* partials[RENDER_PARTIAL_COUNT_MAX];

  /** Partial names parallel to `partials`, used as a compile cache. */
  struct Str partial_names[RENDER_PARTIAL_COUNT_MAX];

  /** Number of populated entries in `partials`/`partial_names`. */
  ptrdiff_t partial_count;

  /** Partial expansions resolved so far, bounded by `RENDER_EXPANSION_COUNT_MAX`. */
  ptrdiff_t expansion_count;

  /**
   * Log of this render alone, which receives its first failure. `template_render_file_limited`
   * relays it to the caller's log, attributed to the album being rendered.
   */
  struct Log* log;

  /**
   * `render_fail` sets this on the first failure in any callback, and
   * `template_render_file_limited` checks it once processing returns. That failure is an exceeded
   * limit, an unescaped interpolation, an unsafe partial name, or an unreadable or uncompilable
   * partial.
   */
  bool has_failed;
};

/**
 * Destination for one render's output, passed to mustache4c as its renderer data. Pairs the buffer
 * and its bound with the provider state so an output callback that passes the bound can record the
 * diagnostic itself.
 */
struct RenderOutput {
  /** Buffer accumulating the rendered bytes, in the caller's `perm` arena. */
  struct Buf* buffer;

  /** Largest accumulated length `buffer` may reach, in bytes. */
  ptrdiff_t output_len_max;

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
 * @return `0` on success, or `-1` on an exceeded output limit.
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
 * @return `0` on success, or `-1` on an exceeded output limit.
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
 * The lookup runs before copying the name into the arena. A partial referenced inside a loop
 * resolves once per iteration, and copying first would leak an arena allocation per occurrence.
 *
 * @param provider_data Render state holding the compile cache. Must not be `NULL`.
 * @param name          Partial name.
 * @return The cached compiled partial, or `NULL` when the name is not cached.
 */
static MUSTACHE_TEMPLATE* node_get_partial_cached(const struct ProviderData* provider_data,
                                                  struct Str name) __attribute__((nonnull(1)));

/**
 * @brief Loads and compiles `partials/<name>.html`, reporting the reason it could not.
 *
 * Reads the file into a copy of the render's scratch, so its bytes are released once mustache4c has
 * compiled them. The compile allocates into that copy too, after the bytes, so a syntax error's
 * diagnostic cannot overwrite the text mustache4c goes on scanning. Does not register the result in
 * the compile cache. The caller owns that bookkeeping.
 *
 * @param provider_data Render state supplying `templates_dir` and the arena, and receiving a
 *                      diagnostic on failure. Must not be `NULL`.
 * @param name          Partial name, already validated as a safe identifier. It still points at the
 *                      bytes mustache4c passed, which `node_get_partial_holder` locates.
 * @param templ_out     Receives the compiled partial template on success only. Must not be `NULL`.
 * @return `0` on success, or `-1` on a path, read, NUL-byte, or compile failure.
 */
static int node_get_partial_compile(struct ProviderData* provider_data,
                                    struct Str name,
                                    MUSTACHE_TEMPLATE** templ_out) __attribute__((nonnull(1, 3)));

/**
 * @brief Builds the `partials/<name>.html` template path rooted under the render's `templates_dir`.
 *
 * @param provider_data     Render state supplying `templates_dir` and the arena. Must not be
 *                          `NULL`.
 * @param name              Partial name, already validated as a safe identifier.
 * @param relative_path_out Receives the `partials/<name>.html` form, arena-owned. Handed back
 *                          rather than rebuilt by the caller because that is the form
 *                          `record_parse_error` names. Must not be `NULL`.
 * @param partial_path_out  Receives the joined path owned by the render arena. Must not be `NULL`.
 */
static void node_get_partial_compile_path(struct ProviderData* provider_data,
                                          struct Str name,
                                          struct Str* relative_path_out,
                                          struct Str* partial_path_out)
    __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Names the template or partial whose `{{>...}}` reference passed `name` to
 *        `node_get_partial`.
 *
 * A bad reference fails while mustache4c renders, and it tells no callback which template it is
 * executing or when a partial ends. The name it passes points into the compiled template holding
 * the reference, though. `mustache_process` reads it at `insns + reg_pc`, and `insns` is the
 * `MUSTACHE_TEMPLATE` pointer itself. `mustache.h` never states that, so this rests on reading
 * `mustache.c`, as `node_get_partial_compile` does. Each compiled template is one live allocation,
 * and live allocations do not overlap, so the holder is the compiled template with the highest
 * start at or below `name`.
 *
 * @param provider_data Render state holding the compiled templates. Must not be `NULL`.
 * @param name          Partial name exactly as mustache4c passed it to `node_get_partial`.
 * @return The holder's path relative to `templates_dir`, which is the form `(in '<file>')` takes.
 */
static struct Str node_get_partial_holder(struct ProviderData* provider_data, const char* name)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Allocates an arena-owned node of the given kind for the current render.
 *
 * Zeroes every field other than `kind`, so a caller sets only the fields its kind carries.
 *
 * @param provider_data Render provider state owning the node arena. Must not be `NULL`.
 * @param kind          Node kind to assign.
 * @return The new node.
 */
static struct Node* node_alloc(struct ProviderData* provider_data, enum NodeKind kind)
    __attribute__((nonnull(1), returns_nonnull));

/**
 * @brief Returns a scalar node wrapping `value`, or `NULL` when `value` is empty.
 *
 * @param provider_data Render provider state owning the node arena. Must not be `NULL`.
 * @param value         String to wrap, borrowed for the rest of the render.
 * @return A scalar node, or `NULL` when `value` is empty.
 */
static struct Node* node_scalar(struct ProviderData* provider_data, struct Str value)
    __attribute__((nonnull(1)));

/**
 * @brief Returns a scalar node holding `value` in decimal.
 *
 * Copies the formatted digits into the render arena, because a scalar node borrows its text for the
 * rest of the render.
 *
 * @param provider_data Render provider state owning the node arena. Must not be `NULL`.
 * @param value         Number to format.
 * @return A scalar node.
 */
static struct Node* node_scalar_number(struct ProviderData* provider_data, ptrdiff_t value)
    __attribute__((nonnull(1)));

/**
 * @brief Returns a scalar node holding an output-root-relative URL rebased onto the current page.
 *
 * Joins the URL behind the rendered album's `path_to_root`, so a link resolves from the page's own
 * directory rather than only from the output root.
 *
 * @param provider_data     Render provider state supplying the rendered album and the node arena.
 *                          Must not be `NULL`.
 * @param root_relative_url Output-root-relative URL, already URL-safe because it is built from
 *                          slugs.
 * @return A scalar node, or `NULL` when the rebased URL is empty.
 */
static struct Node* node_scalar_url(struct ProviderData* provider_data,
                                    struct Str root_relative_url) __attribute__((nonnull(1)));

/**
 * @brief Resolves a `gallery.*` name against the render's gallery configuration.
 *
 * @param provider_data Render provider state holding the gallery configuration. Must not be `NULL`.
 * @param name          Requested field name.
 * @return The resolved node, or `NULL` when the name is unmatched or its value is unset.
 */
static struct Node* resolve_gallery(struct ProviderData* provider_data, struct Str name)
    __attribute__((nonnull(1)));

/**
 * @brief Resolves a bare field name against an album.
 *
 * `has_media` and `has_sub_albums` resolve to a truthy scalar only when the album holds that
 * content, so a section over either renders exactly when it is non-empty. A URL resolves already
 * rebased onto the page being rendered.
 *
 * @param provider_data Render provider state. Must not be `NULL`.
 * @param album         Album to resolve against. Must not be `NULL`.
 * @param name          Requested field name.
 * @return The resolved node, or `NULL` when the name is unmatched or the field is absent.
 */
static struct Node* resolve_album(struct ProviderData* provider_data,
                                  const struct Album* album,
                                  struct Str name) __attribute__((nonnull(1, 2)));

/**
 * @brief Resolves a bare field name against a media item, including its derivative names.
 *
 * A name matching no media field is matched against the item's configured derivative names, so
 * `{{m.url}}` reaches the derivative named `m`. `is_image` and `is_video` resolve to a truthy
 * scalar only for that source kind, so a section over either renders exactly for it.
 *
 * @param provider_data Render provider state. Must not be `NULL`.
 * @param media         Media item to resolve against. Must not be `NULL`.
 * @param name          Requested field name.
 * @return The resolved node, or `NULL` when the name is unmatched or the field is absent.
 */
static struct Node* resolve_media(struct ProviderData* provider_data,
                                  const struct MediaItem* media,
                                  struct Str name) __attribute__((nonnull(1, 2)));

/**
 * @brief Resolves a bare field name against one derivative of a media item.
 *
 * @param provider_data Render provider state. Must not be `NULL`.
 * @param derivative    Derivative to resolve against. Must not be `NULL`.
 * @param name          Requested field name.
 * @return The resolved node, or `NULL` when the name is unmatched.
 */
static struct Node* resolve_derivative(struct ProviderData* provider_data,
                                       const struct MediaDerivative* derivative,
                                       struct Str name) __attribute__((nonnull(1, 2)));

/**
 * @brief Records the render's first failure and marks the render as failed.
 *
 * Only the first message is kept: it names the root cause, and later callbacks typically fail as a
 * consequence of it.
 *
 * @param provider_data Render provider state holding the log and failure flag. Must not be `NULL`.
 * @param file          Template the failure was found in, or empty to name none.
 * @param format        `printf` format of the failure, without the attribution. Must not be `NULL`.
 * @param ...           Arguments that the format converts.
 */
static void render_fail(struct ProviderData* provider_data,
                        struct Str file,
                        const char* format,
                        ...) __attribute__((format(printf, 3, 4), nonnull(1, 3)));

/**
 * @brief Relays a finished render's failure to the caller's log.
 *
 * An album page render names the album it was rendering, because the template a failure is in is
 * shared by every page: `<failure> (while rendering album '<source_dir>')`, or
 * `<failure> (while rendering the root album)`. The parenthesis keeps the attribution from reading
 * as part of a failure that itself ends in a `: <reason>` clause. An aggregate render relays the
 * failure as it stands.
 *
 * @param context    Render context, whose `album_current` is the album being rendered when
 *                   `is_album_page` is set. Must not be `NULL`.
 * @param render_log Log of the render, holding at most one failure. Must not be `NULL`.
 * @param log        Caller's log. Must not be `NULL`.
 */
static void template_render_file_limited_report(const struct TemplateContext* context,
                                                const struct Log* render_log,
                                                struct Log* log) __attribute__((nonnull(1, 2, 3)));

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
 * Renderer dispatch table. These tables are `const` so they stay in read-only data: renders run
 * concurrently in pool jobs, and no state a render touches is a writable global.
 */
static const MUSTACHE_RENDERER renderer = {out_verbatim, out_escaped};

/** Data-provider dispatch table. */
static const MUSTACHE_DATAPROVIDER provider = {node_dump, node_get_root, node_get_child_by_name,
                                               node_get_child_by_index, node_get_partial};

/** Parser dispatch table. */
static const MUSTACHE_PARSER parser = {record_parse_error};

int template_render_file(struct Str templates_dir,
                         struct Str template_name,
                         const struct TemplateContext* context,
                         struct Os* os,
                         struct Arena* perm,
                         struct Arena scratch,
                         struct Str* html_out,
                         struct Log* log) {
  return template_render_file_limited(templates_dir, template_name, context, RENDER_OUTPUT_LEN_MAX,
                                      os, perm, scratch, html_out, log);
}

int template_render_file_limited(struct Str templates_dir,
                                 struct Str template_name,
                                 const struct TemplateContext* context,
                                 ptrdiff_t output_len_max,
                                 struct Os* os,
                                 struct Arena* perm,
                                 struct Arena scratch,
                                 struct Str* html_out,
                                 struct Log* log) {
  // The render logs into a log of its own, which grows in the caller's log's arena, so it shares no
  // arena with anything the render copies by value. That lets the relay below attribute the failure
  // once, wherever in the render it happened.
  struct Log render_log = {.buf = {.arena = log->buf.arena}};
  // Every template name was checked as a safe relative name when its input was loaded.
  assert(path_is_safe_relative(template_name));

  // The output grows in `perm`, where nothing else in the render allocates, so it extends in place.
  // Every other allocation goes into `scratch`, this call's copy of the caller's region, which is
  // released when the call returns. Renders run concurrently in pool jobs, each with arenas of its
  // own worker. Nothing in this file may allocate into an arena reached through `context`.
  struct Buf buffer = {.arena = perm};
  struct Node node_root = {.kind = NODE_ROOT};
  // Because this literal names only the non-zero values, initialization cannot accidentally omit a
  // new `ProviderData` field. `partial_count`, `expansion_count`, and `has_failed` start at zero.
  // The code assigns `partials` and `partial_names` when a partial compiles, and `templ` once the
  // template compiles. It assigns `template_name_compiling` before both `mustache_compile` calls.
  // Only those calls can reach `record_parse_error`.
  struct ProviderData provider_data = {.os = os,
                                       .templates_dir = templates_dir,
                                       .template_name = template_name,
                                       .context = context,
                                       .arena = &scratch,
                                       .node_root = &node_root,
                                       .log = &render_log};
  struct RenderOutput render_output = {
      .buffer = &buffer, .output_len_max = output_len_max, .provider_data = &provider_data};

  struct Str template_data = {0};
  MUSTACHE_TEMPLATE* templ = NULL;
  int rc = -1;

  const struct Str template_path = path_join(templates_dir, template_name, &scratch);
  if (os_read(os, template_path, TEMPLATE_FILE_LEN_MAX, &scratch, &template_data,
              S("failed to read template"), &render_log) != 0) {
    goto cleanup;
  }
  if (!text_is_nul_free(template_data)) {
    (void)log_error(&render_log, "failed to read template: contains an embedded NUL byte ('%.*s')",
                    STR_FMT(template_path));
    goto cleanup;
  }

  provider_data.template_name_compiling = template_name;
  templ =
      mustache_compile(template_data.data, (size_t)template_data.len, &parser, &provider_data, 0);
  if (templ == NULL) {
    // `record_parse_error` has already named the reason and its line for a syntax error. mustache4c
    // fails a compile without reporting one only when its own allocation fails.
    if (!provider_data.has_failed) {
      os_oom();
    }
    goto cleanup;
  }
  provider_data.templ = templ;
  // The compile copied what the render needs. Nothing in the render reuses the template's pages, so
  // they are given back, where a `malloc` design would free the bytes and reuse them for the
  // output.
  arena_discard(&scratch, template_data.data, template_data.len);
  // The render needs both failure signals. `mustache_process` reports only a callback that returned
  // non-zero, and a `NULL` from `node_get_partial` or `node_get_child_by_name` means *absent* to
  // mustache4c rather than *failed*. It skips the partial and renders on. Without `has_failed` an
  // unreadable partial would yield a successful, silently incomplete page. This line is also the
  // boundary where mustache4c's zero/non-zero convention becomes this project's `0`/`-1` action
  // contract.
  if (mustache_process(templ, &renderer, &render_output, &provider, &provider_data) != 0 ||
      provider_data.has_failed) {
    // A failing callback has already recorded the specific cause. mustache4c fails a render without
    // one only when its own allocation fails.
    if (!provider_data.has_failed) {
      os_oom();
    }
    goto cleanup;
  }
  *html_out = buf_str(&buffer);
  rc = 0;

cleanup:
  // The compiled templates are mustache4c's own `malloc` allocations, so they are the one thing a
  // render releases by hand.
  for (ptrdiff_t i = 0; i < provider_data.partial_count; i++) {
    mustache_release(provider_data.partials[i]);
  }
  if (templ != NULL) {
    mustache_release(templ);
  }
  template_render_file_limited_report(context, &render_log, log);
  return rc;
}

static int out_verbatim(const char* output, size_t output_len, void* renderer_data) {
  struct RenderOutput* render_output = renderer_data;
  buf_append(render_output->buffer, (struct Str){output, (ptrdiff_t)output_len});
  return check_output_size(render_output);
}

static int out_escaped(const char* output, size_t output_len, void* renderer_data) {
  struct RenderOutput* render_output = renderer_data;
  html_escape_append(render_output->buffer, (struct Str){output, (ptrdiff_t)output_len});
  return check_output_size(render_output);
}

static int check_output_size(const struct RenderOutput* render_output) {
  const ptrdiff_t output_len = render_output->buffer->len;
  if (output_len <= render_output->output_len_max) {
    return 0;
  }
  render_fail(render_output->provider_data, render_output->provider_data->template_name,
              "render exceeds max rendered output (%td bytes) at %td bytes",
              render_output->output_len_max, output_len);
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
    render_fail(provider_data, provider_data->template_name,
                "unescaped Mustache interpolation is not supported");
    return -1;
  }
  assert(node->scalar.len >= 0);
  return out_fn(node->scalar.data, (size_t)node->scalar.len, renderer_data);
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
  const struct Str name_text = {name, (ptrdiff_t)name_len};
  switch (node->kind) {
    case NODE_ROOT:
      if (str_eq(name_text, S("gallery"))) {
        return node_alloc(provider_data, NODE_GALLERY);
      }
      if (str_eq(name_text, S("album"))) {
        struct Node* child = node_alloc(provider_data, NODE_ALBUM);
        child->album = provider_data->context->album_current;
        return child;
      }
      if (str_eq(name_text, S("sub_albums"))) {
        return node_alloc(provider_data, NODE_ALBUM_LIST);
      }
      if (str_eq(name_text, S("media"))) {
        return node_alloc(provider_data, NODE_MEDIA_LIST);
      }
      if (str_eq(name_text, S("breadcrumbs"))) {
        return node_alloc(provider_data, NODE_BREADCRUMB_LIST);
      }
      if (str_eq(name_text, S("generator"))) {
        return node_scalar(provider_data, fram_generator_string());
      }
      return resolve_album(provider_data, provider_data->context->album_current, name_text);
    case NODE_GALLERY:
      return resolve_gallery(provider_data, name_text);
    case NODE_ALBUM:
      return resolve_album(provider_data, node->album, name_text);
    case NODE_MEDIA:
      return resolve_media(provider_data, node->media, name_text);
    case NODE_DERIVATIVE:
      return resolve_derivative(provider_data, node->derivative, name_text);
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
    child->album = current->sub_albums[index];
    return child;
  }
  if (node->kind == NODE_MEDIA_LIST) {
    if (index >= current->media_count) {
      return NULL;
    }
    struct Node* child = node_alloc(provider_data, NODE_MEDIA);
    child->media = current->media[index];
    return child;
  }
  if (node->kind == NODE_BREADCRUMB_LIST) {
    if (index > current->depth) {
      return NULL;
    }
    const struct Album* crumb = current;
    for (ptrdiff_t remaining = current->depth - index; remaining > 0; remaining--) {
      crumb = crumb->parent;
    }
    struct Node* child = node_alloc(provider_data, NODE_ALBUM);
    child->album = crumb;
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
  const struct Str name_text = {name, (ptrdiff_t)name_len};
  // mustache4c renders on past a callback's failure, so a failed render resolves no more partials.
  // That keeps a later partial's read failure from adding a second message after the first.
  if (provider_data->has_failed) {
    return NULL;
  }

  // The counter increments before the cache lookup, so the bound holds for a cycle whose partial is
  // already compiled. This is the render's only termination guard.
  provider_data->expansion_count++;
  if (provider_data->expansion_count > RENDER_EXPANSION_COUNT_MAX) {
    render_fail(provider_data, provider_data->template_name,
                "render exceeds max partial expansions (%d); check for a partial that includes "
                "itself",
                RENDER_EXPANSION_COUNT_MAX);
    return NULL;
  }

  // The cache lookup precedes the arena copy below, so a repeated reference costs no allocation.
  MUSTACHE_TEMPLATE* cached = node_get_partial_cached(provider_data, name_text);
  if (cached != NULL) {
    return cached;
  }

  if (!text_is_safe_identifier(name_text)) {
    // This is the directory-traversal guard, so it cannot be dropped as redundant input validation.
    // The partial name becomes a filesystem path, so this check validates it here rather than
    // trusting it to mustache4c. `mustache_validate_tagname` happens to reject `..` and to accept
    // `/`, and neither rule is part of `mustache.h`'s contract, so `{{>a/b}}` does reach this
    // callback and only this check keeps a partial inside `partials/`.
    render_fail(provider_data, node_get_partial_holder(provider_data, name),
                "partial name must contain only letters, digits, '_' and '-': '%.*s'",
                STR_FMT(name_text));
    return NULL;
  }
  if (provider_data->partial_count >= RENDER_PARTIAL_COUNT_MAX) {
    render_fail(provider_data, provider_data->template_name,
                "render exceeds max distinct partials (%d) while resolving partial '%.*s'",
                RENDER_PARTIAL_COUNT_MAX, STR_FMT(name_text));
    return NULL;
  }

  MUSTACHE_TEMPLATE* templ = NULL;
  if (node_get_partial_compile(provider_data, name_text, &templ) != 0) {
    return NULL;
  }
  // mustache4c's name is valid only during this call, so the cache keeps a copy.
  provider_data->partials[provider_data->partial_count] = templ;
  provider_data->partial_names[provider_data->partial_count] =
      str_clone(name_text, provider_data->arena);
  provider_data->partial_count++;
  return templ;
}

static MUSTACHE_TEMPLATE* node_get_partial_cached(const struct ProviderData* provider_data,
                                                  struct Str name) {
  for (ptrdiff_t i = 0; i < provider_data->partial_count; i++) {
    if (str_eq(name, provider_data->partial_names[i])) {
      return provider_data->partials[i];
    }
  }
  return NULL;
}

static int node_get_partial_compile(struct ProviderData* provider_data,
                                    struct Str name,
                                    MUSTACHE_TEMPLATE** templ_out) {
  struct Str relative_path = {0};
  struct Str partial_path = {0};
  node_get_partial_compile_path(provider_data, name, &relative_path, &partial_path);
  // The bytes go into a copy of the render's scratch, which drops them when this returns, and the
  // render's own arena allocates over them afterwards.
  struct Arena partial_scratch = *provider_data->arena;
  struct Str partial_data = {0};
  // The read logs its own failure, which is this render's first, because a failed render resolves
  // no partials.
  if (os_read(provider_data->os, partial_path, TEMPLATE_FILE_LEN_MAX, &partial_scratch,
              &partial_data, S("failed to read partial"), provider_data->log) != 0) {
    provider_data->has_failed = true;
    return -1;
  }
  if (!text_is_nul_free(partial_data)) {
    render_fail(provider_data, (struct Str){0},
                "failed to read partial: contains an embedded NUL byte ('%.*s')",
                STR_FMT(partial_path));
    return -1;
  }

  // This uses the relative path, not the bare `name`. `record_parse_error` composes this into
  // `(in '<file>')`, which must denote a file.
  provider_data->template_name_compiling = relative_path;
  // Dropping the bytes once this returns is safe because `mustache_compile` copies every literal
  // run and tag name into the instruction buffer it returns. `mustache.h` never states that
  // lifetime, so this rests on reading `mustache.c`. A vendor that started borrowing `templ_data`
  // would turn this into a use-after-free for the rest of the render.
  //
  // mustache4c goes on scanning `partial_data` after it reports a syntax error, and `render_fail`
  // allocates that error's diagnostic. The render's arena has not moved past `partial_data`, so a
  // diagnostic allocated there would overwrite those bytes. For the compile the provider allocates
  // from `partial_scratch` instead, after them.
  struct Arena* render_arena = provider_data->arena;
  provider_data->arena = &partial_scratch;
  MUSTACHE_TEMPLATE* templ =
      mustache_compile(partial_data.data, (size_t)partial_data.len, &parser, provider_data, 0);
  provider_data->arena = render_arena;
  if (templ == NULL) {
    // `record_parse_error` names a syntax error itself. mustache4c fails a compile without
    // reporting one only when its own allocation fails.
    if (!provider_data->has_failed) {
      os_oom();
    }
    return -1;
  }
  *templ_out = templ;
  return 0;
}

static void node_get_partial_compile_path(struct ProviderData* provider_data,
                                          struct Str name,
                                          struct Str* relative_path_out,
                                          struct Str* partial_path_out) {
  struct Arena* arena = provider_data->arena;
  const struct Str relative_path =
      str_concat(str_concat(S("partials/"), name, arena), S(".html"), arena);
  *relative_path_out = relative_path;
  *partial_path_out = path_join(provider_data->templates_dir, relative_path, arena);
}

static struct Str node_get_partial_holder(struct ProviderData* provider_data, const char* name) {
  // The addresses compare as integers, because `<` between pointers into different objects is
  // undefined.
  const uintptr_t name_address = (uintptr_t)name;
  uintptr_t holder_address = (uintptr_t)provider_data->templ;
  ptrdiff_t holder_index = -1;
  for (ptrdiff_t i = 0; i < provider_data->partial_count; i++) {
    const uintptr_t partial_address = (uintptr_t)provider_data->partials[i];
    if (partial_address <= name_address &&
        (holder_address > name_address || partial_address > holder_address)) {
      holder_address = partial_address;
      holder_index = i;
    }
  }
  if (holder_index < 0) {
    return provider_data->template_name;
  }
  struct Arena* arena = provider_data->arena;
  return str_concat(str_concat(S("partials/"), provider_data->partial_names[holder_index], arena),
                    S(".html"), arena);
}

static struct Node* node_alloc(struct ProviderData* provider_data, enum NodeKind kind) {
  // This allocates a fresh node per resolution, never a shared singleton, even for the kinds that
  // carry no per-instance state. `vendor/mustache4c/mustache.h` requires each node of the hierarchy
  // to be uniquely identified by its pointer. Today's `mustache.c` only pushes and pops them, so
  // sharing would work, but that is an implementation detail a vendor bump can change without a
  // word. The allocation reads as an unnoticed inefficiency, so do not replace it with a shared
  // node.
  struct Node* node = arena_new(provider_data->arena, 1, struct Node);
  node->kind = kind;
  return node;
}

static struct Node* node_scalar(struct ProviderData* provider_data, struct Str value) {
  // An empty value is absent, not present-and-empty, so `{{#field}}` skips it and `{{^field}}`
  // fires. mustache4c decides a section's truthiness by asking for child 0. A non-list node answers
  // with itself, so returning a node here for `""` would make a section over an unset field always
  // render. Interpolation is unchanged. `{{field}}` writes zero bytes whether the node is absent or
  // holds an empty string. `resolve_album` already applies this same rule to `has_media` and
  // `has_sub_albums`.
  if (value.len == 0) {
    return NULL;
  }
  struct Node* node = node_alloc(provider_data, NODE_SCALAR);
  node->scalar = value;
  return node;
}

static struct Node* node_scalar_number(struct ProviderData* provider_data, ptrdiff_t value) {
  char number[SCALAR_NUMBER_SIZE];
  const int number_len = snprintf(number, sizeof(number), "%td", value);
  return node_scalar(provider_data,
                     str_clone((struct Str){number, number_len}, provider_data->arena));
}

static struct Node* node_scalar_url(struct ProviderData* provider_data,
                                    struct Str root_relative_url) {
  return node_scalar(provider_data, path_join(provider_data->context->album_current->path_to_root,
                                              root_relative_url, provider_data->arena));
}

static struct Node* resolve_gallery(struct ProviderData* provider_data, struct Str name) {
  const struct GalleryConfig* gallery_config = provider_data->context->gallery_config;
  if (str_eq(name, S("title"))) {
    return node_scalar(provider_data, gallery_config->title);
  }
  if (str_eq(name, S("author"))) {
    return node_scalar(provider_data, gallery_config->author);
  }
  if (str_eq(name, S("base_url"))) {
    return node_scalar(provider_data, gallery_config->base_url);
  }
  return NULL;
}

static struct Node* resolve_album(struct ProviderData* provider_data,
                                  const struct Album* album,
                                  struct Str name) {
  if (str_eq(name, S("title"))) {
    return node_scalar(provider_data, album->title);
  }
  if (str_eq(name, S("url"))) {
    return node_scalar_url(provider_data, album->url_path);
  }
  if (str_eq(name, S("path_to_root"))) {
    return node_scalar(provider_data, album->path_to_root);
  }
  if (str_eq(name, S("item_count"))) {
    return node_scalar_number(provider_data, album->item_count_total);
  }
  if (str_eq(name, S("cover"))) {
    if (album->cover == NULL) {
      return NULL;
    }
    struct Node* cover = node_alloc(provider_data, NODE_MEDIA);
    cover->media = album->cover;
    return cover;
  }
  if (str_eq(name, S("has_media"))) {
    return album->media_count == 0 ? NULL : node_scalar(provider_data, S("true"));
  }
  if (str_eq(name, S("has_sub_albums"))) {
    return album->sub_album_count == 0 ? NULL : node_scalar(provider_data, S("true"));
  }
  if (str_eq(name, S("parent"))) {
    if (album->parent == NULL) {
      return NULL;
    }
    struct Node* parent = node_alloc(provider_data, NODE_ALBUM);
    parent->album = album->parent;
    return parent;
  }
  return NULL;
}

static struct Node* resolve_media(struct ProviderData* provider_data,
                                  const struct MediaItem* media,
                                  struct Str name) {
  if (str_eq(name, S("title"))) {
    return node_scalar(provider_data, media->title);
  }
  if (str_eq(name, S("is_video"))) {
    return media->kind == MEDIA_KIND_VIDEO ? node_scalar(provider_data, S("true")) : NULL;
  }
  if (str_eq(name, S("is_image"))) {
    return media->kind == MEDIA_KIND_IMAGE ? node_scalar(provider_data, S("true")) : NULL;
  }
  if (str_eq(name, S("original_url"))) {
    return node_scalar_url(provider_data, media->original_url);
  }
  if (str_eq(name, S("duration"))) {
    return node_scalar_number(provider_data, media->duration_ms);
  }
  for (ptrdiff_t i = 0; i < media->derivative_count; i++) {
    const struct MediaDerivative* derivative = &media->derivatives[i];
    if (str_eq(name, derivative->name)) {
      struct Node* child = node_alloc(provider_data, NODE_DERIVATIVE);
      child->derivative = derivative;
      return child;
    }
  }
  return NULL;
}

static struct Node* resolve_derivative(struct ProviderData* provider_data,
                                       const struct MediaDerivative* derivative,
                                       struct Str name) {
  if (str_eq(name, S("url"))) {
    return node_scalar_url(provider_data, derivative->url);
  }
  if (str_eq(name, S("width"))) {
    return node_scalar_number(provider_data, derivative->width_px);
  }
  if (str_eq(name, S("height"))) {
    return node_scalar_number(provider_data, derivative->height_px);
  }
  return NULL;
}

static void render_fail(struct ProviderData* provider_data,
                        struct Str file,
                        const char* format,
                        ...) {
  if (!provider_data->has_failed) {
    // The first pass measures the message and the second writes it into the render's scratch, as
    // `buf_printf` does. Starting the list twice needs no `va_copy`, and forwarding it to
    // `log_error_v` instead makes Clang's analyzer report a false `va_end` on AArch64 Linux.
    va_list args;
    va_start(args, format);
    const int message_len = vsnprintf(NULL, 0, format, args);
    va_end(args);
    assert(message_len >= 0);
    char* message = arena_new(provider_data->arena, (ptrdiff_t)message_len + 1, char);
    va_start(args, format);
    (void)vsnprintf(message, (size_t)message_len + 1, format, args);
    va_end(args);
    (void)log_error_in(provider_data->log, file, "%.*s", message_len, message);
  }
  provider_data->has_failed = true;
}

static void template_render_file_limited_report(const struct TemplateContext* context,
                                                const struct Log* render_log,
                                                struct Log* log) {
  const struct Album* album = context->album_current;
  if (render_log->errors == 0 || !context->is_album_page) {
    log_append(log, render_log);
    return;
  }
  // The render records one failure, a line ending in `\n`, which the attribution goes in front of.
  const struct Str failure = buf_str(&render_log->buf);
  const struct Str failure_line = str_head(failure, failure.len - 1);
  if (album_is_root(album)) {
    (void)log_error(log, "%.*s" TEMPLATE_RENDER_ATTRIBUTION "the root album)",
                    STR_FMT(failure_line));
    return;
  }
  (void)log_error(log, "%.*s" TEMPLATE_RENDER_ATTRIBUTION "album '%.*s')", STR_FMT(failure_line),
                  STR_FMT(album->source_dir));
}

static void record_parse_error(int err_code,
                               const char* msg,
                               unsigned line,
                               unsigned column,
                               void* parser_data) {
  (void)err_code;
  struct ProviderData* provider_data = parser_data;
  render_fail(provider_data, provider_data->template_name_compiling, "%s at line %u, column %u",
              msg, line, column);
}
