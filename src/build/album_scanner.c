#include "build/album_scanner.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "build/job.h"
#include "build/output_path.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/text.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/media_item.h"
#include "formats/exif.h"
#include "formats/image.h"
#include "runtime/memory.h"
#include "runtime/os.h"
#include "runtime/video.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** Temporary indexing state while a skeleton is assembled. */
struct SkeletonState {
  /**
   * Arena for everything the caller keeps: every album, media item and string, and the arrays
   * handed back. It is a copy of the caller's `perm`, committed only when the scan succeeds.
   */
  struct Arena perm;

  /**
   * Albums created so far, kept sorted by `source_dir` for `find_album`'s binary search. The array
   * grows in `perm` and is handed to the caller. The copies a growth leaves behind add up to less
   * than the final array.
   */
  struct Album** albums;

  /** Number of albums in `albums`, with the root album at index `0`. */
  ptrdiff_t album_count;

  /** Allocated slots in `albums`. */
  ptrdiff_t album_capacity;

  /** One media item per source path, in source order, handed to the caller on success. */
  struct MediaItem** media;

  /**
   * Album owning each entry of `media`, at the same index. `finish_relationships` reads it to
   * attach items to albums. It lives in the scan's scratch, so it is not handed to the caller.
   */
  struct Album** media_albums;

  /** Number of entries in `media` and `media_albums`, fixed at the source path count. */
  ptrdiff_t media_count;
};

/**
 * Worker context shared by all probe jobs. Every field is read-only to a job, except
 * `video_frames`, where each job writes its own slot, and `media`, where each job writes its own
 * item's probe fields: `duration_ms`, `orientation`, `width_px`, `height_px`, and each derivative's
 * dimensions.
 */
struct ProbeContext {
  /** Configured derivatives each probed item is sized against. Read-only during the run. */
  const struct GalleryConfig* gallery_config;

  /**
   * Media items to probe, one per job index. The array is read-only during the run. A job writes
   * the probe fields of the single item it owns and allocates nothing in the skeleton's arena.
   */
  struct MediaItem* const* media;

  /**
   * One frame slot per item, whose file the probe creates for a video. A job writes only
   * `video_frames[index]`, and only its `is_created` and the name `mkostemp` fills in.
   */
  struct VideoFrame* video_frames;

  /** Host every job reads its source or frame from. */
  struct Os* os;
};

/**
 * @brief Inserts an album into the skeleton's `source_dir`-sorted album array.
 *
 * Growing the array and choosing the insertion position both happen here, so `albums` stays sorted
 * for `find_album`'s binary search.
 *
 * @param state Skeleton state owning the album array. Must not be `NULL`.
 * @param album Album to insert. Must not be `NULL`.
 */
static void insert_album(struct SkeletonState* state, struct Album* album)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Finds the album whose `source_dir` matches exactly.
 *
 * @param albums      Array sorted by `source_dir`, as `insert_album` maintains. Must not be `NULL`.
 * @param album_count Number of entries in `albums`.
 * @param source_dir  Directory relative to `input_dir` to look for, empty for the root.
 * @return The matching album, or `NULL` when no album carries that `source_dir`.
 */
static struct Album* find_album(struct Album* const* albums,
                                ptrdiff_t album_count,
                                struct Str source_dir) __attribute__((nonnull(1)));

/**
 * @brief Binary-searches for the first album whose `source_dir` does not sort before `source_dir`.
 *
 * Source paths are sorted as whole paths, and `a-b/x.jpg` sorts before `a/y.jpg` while `a` sorts
 * before `a-b` as a directory, so albums are not created in `source_dir` order and each insertion
 * has to search for its place.
 *
 * @param albums      Array sorted by `source_dir`. Must not be `NULL`.
 * @param album_count Number of entries in `albums`.
 * @param source_dir  Directory relative to `input_dir` to place.
 * @return Index of that album, or `album_count` when every album sorts before `source_dir`.
 */
static ptrdiff_t find_album_position(struct Album* const* albums,
                                     ptrdiff_t album_count,
                                     struct Str source_dir) __attribute__((nonnull(1)));

/**
 * @brief Allocates one album and derives every path, title, and depth field it owns.
 *
 * The album and its strings go into `arena`, and the page path they are derived from into
 * `scratch`. The generated page path is checked against the output-path limits here, so an overlong
 * path fails at creation rather than at write time. The album is created even then and marked
 * rejected, so the walk finds it again and reports the limit once rather than once per source or
 * sub-album below it. A sub-album of a rejected album is rejected without a report of its own. The
 * album is not linked into `parent`. `finish_relationships` builds the child arrays.
 *
 * @param gallery_config Gallery configuration supplying the gallery title and output directory.
 *                       Must not be `NULL`.
 * @param source_dir     Directory relative to `input_dir`, empty for the root album.
 * @param parent         Album whose `slug_path` prefixes this one, or `NULL` for the root.
 * @param arena          Arena that owns the album and its strings. Must not be `NULL`.
 * @param scratch        Arena for the page path relative to `output_dir`.
 * @param album_out      Receives the new album. Must not be `NULL`.
 * @param log            Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` on an output-limit rejection of the album or of `parent`.
 */
static int make_album(const struct GalleryConfig* gallery_config,
                      struct Str source_dir,
                      struct Album* parent,
                      struct Arena* arena,
                      struct Arena scratch,
                      struct Album** album_out,
                      struct Log* log) __attribute__((nonnull(1, 4, 6, 7)));

/**
 * @brief Resolves a source directory to its album, creating every missing ancestor on the way.
 *
 * Walks `source_dir` one `/`-separated component at a time from the root, reusing an album that
 * already exists and inserting a newly created one otherwise. `state->albums[0]` must already hold
 * the root album.
 *
 * @param state          Skeleton state whose album array is searched and extended. Must not be
 *                       `NULL`.
 * @param gallery_config Gallery configuration passed to each created album. Must not be `NULL`.
 * @param source_dir     Directory relative to `input_dir`, empty for the root.
 * @param scratch        Arena for each created album's temporaries.
 * @param album_out      Receives the album for `source_dir`, owned by `state`. Must not be `NULL`.
 * @param log            Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` when an album it created exceeds an output-path limit.
 */
static int ensure_source_albums(struct SkeletonState* state,
                                const struct GalleryConfig* gallery_config,
                                struct Str source_dir,
                                struct Arena scratch,
                                struct Album** album_out,
                                struct Log* log) __attribute__((nonnull(1, 2, 5, 6)));

/**
 * @brief Builds one media item and every derivative and original output path it needs.
 *
 * Allocates the item and the strings it keeps in `state->perm`, and the names and relative paths
 * they are derived from in `scratch`. It publishes the item at `state->media[media_index]`, and
 * counts it on `album`. Only the serial fields are filled. The probe phase writes the dimensions.
 * Each generated output path is checked against the output-path limits here, every one of them even
 * after one fails. The item keeps one `MediaDerivative` per configured derivative, in configuration
 * order, so the two arrays can be read at the same index later.
 *
 * @param state           Skeleton state whose flat media array receives the item. Must not be
 *                        `NULL`.
 * @param gallery_config  Gallery configuration supplying the derivatives and output directory. Must
 *                        not be `NULL`.
 * @param album           Album the item belongs to, which counts it. Must not be `NULL`.
 * @param media_index     Slot in `state->media` to publish to. Must be below `state->media_count`.
 * @param source_path     Full source path, stored on the item and named in diagnostics.
 * @param source_relative Same path relative to `input_dir`, whose basename names the item.
 * @param recipe_root     Derivative output subtree, `_fram/<recipe>`, relative to `output_dir`.
 * @param scratch         Arena for the item's intermediate names and relative paths.
 * @param log             Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` for an unsupported extension, an output-limit rejection, or a
 *         rejected `album`, which reported the limit already.
 */
static int add_media(struct SkeletonState* state,
                     const struct GalleryConfig* gallery_config,
                     struct Album* album,
                     ptrdiff_t media_index,
                     struct Str source_path,
                     struct Str source_relative,
                     struct Str recipe_root,
                     struct Arena scratch,
                     struct Log* log) __attribute__((nonnull(1, 2, 3, 9)));

/**
 * @brief Links albums to their children, attaches media, and computes covers and totals.
 *
 * Runs once every album and media item exists. It counts each album's children, allocates the
 * per-album child and media arrays in `state->perm`, fills them in source order, reverses each
 * array whose configured order is descending, then accumulates `item_count_total` and `cover` from
 * the deepest album upwards. It leaves `state->albums` and `state->media` in source order.
 * `state->albums[0]` must be the root, and `state->media_albums[i]` must name the album owning
 * `state->media[i]`.
 *
 * @param state          Skeleton state holding every album and the media-to-album mapping. Must not
 *                       be `NULL`.
 * @param gallery_config Gallery configuration supplying the album and media orders. Must not be
 *                       `NULL`.
 */
static void finish_relationships(struct SkeletonState* state,
                                 const struct GalleryConfig* gallery_config)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reverses an array of media pointers in place.
 *
 * @param media       Array of `media_count` pointers. May be `NULL` only when `media_count` is 0.
 * @param media_count Number of pointers in `media`.
 */
static void reverse_media(struct MediaItem** media, ptrdiff_t media_count);

/**
 * @brief Reverses an array of album pointers in place.
 *
 * @param albums      Array of `album_count` pointers. May be `NULL` only when `album_count` is 0.
 * @param album_count Number of pointers in `albums`.
 */
static void reverse_albums(struct Album** albums, ptrdiff_t album_count);

/**
 * @brief Builds the `<slug>-<name>.jpg` filename for one derivative.
 *
 * @param slug            Slugified source basename, without its extension.
 * @param derivative_name Configured derivative name.
 * @param arena           Arena that owns the result. Must not be `NULL`.
 * @return Filename owned by `arena`.
 */
static struct Str make_derivative_name(struct Str slug,
                                       struct Str derivative_name,
                                       struct Arena* arena) __attribute__((nonnull(3)));

/**
 * @brief Builds an album's URL-safe output directory from its basename under its parent.
 *
 * Only the last component is slugified. The ancestors come from `parent->slug_path`, which was
 * built the same way, so each component is slugified exactly once.
 *
 * @param source_dir Directory relative to `input_dir`, empty for the root.
 * @param parent     Album supplying the prefix, or `NULL` for a top-level album.
 * @param arena      Arena that owns the result. Must not be `NULL`.
 * @return Slug path owned by `arena`, empty for the root.
 */
static struct Str make_album_slug_path(struct Str source_dir,
                                       const struct Album* parent,
                                       struct Arena* arena) __attribute__((nonnull(3)));

/**
 * @brief Derives an album's display title from its directory basename.
 *
 * @param source_dir    Directory relative to `input_dir`, empty for the root.
 * @param gallery_title Title used for the root album.
 * @param arena         Arena that owns the result. Must not be `NULL`.
 * @return Title owned by `arena`, or `gallery_title` itself for the root.
 */
static struct Str make_album_title(struct Str source_dir,
                                   struct Str gallery_title,
                                   struct Arena* arena) __attribute__((nonnull(3)));

/**
 * @brief Copies text with every underscore replaced by a space.
 *
 * @param text  Source text.
 * @param arena Arena that owns the copy. Must not be `NULL`.
 * @return Copy owned by `arena`.
 */
static struct Str humanize_underscores(struct Str text, struct Arena* arena)
    __attribute__((nonnull(2)));

/**
 * @brief Counts the directory edges a relative path lies below the root.
 *
 * @param path Relative directory path with `/` separators.
 * @return `0` for an empty path, otherwise one more than its separator count.
 */
static ptrdiff_t path_depth(struct Str path);

/**
 * @brief Classifies a media source by its filename extension, folding ASCII case.
 *
 * @param file_name Bare source filename.
 * @param kind_out  Receives `MEDIA_KIND_IMAGE` for `.jpg` or `.jpeg` and `MEDIA_KIND_VIDEO` for
 *                  `.mp4`. Left unchanged for any other extension. Must not be `NULL`.
 * @return `0` when the extension is one this program supports, or `-1` otherwise.
 */
static int classify_media(struct Str file_name, enum MediaKind* kind_out)
    __attribute__((nonnull(2)));

/**
 * @brief Probes one media item's source dimensions and sizes each of its derivatives.
 *
 * This is the `JobFn` of the probe phase, so it runs concurrently with other indexes and writes
 * only the slots belonging to `index`: the item's scalar probe fields, its derivative dimensions,
 * and, for a video, `video_frames[index]`. It allocates nothing in the skeleton's arena: the bytes
 * it reads go into a copy of its worker's scratch. A failure goes to the job's own log.
 *
 * @param index    Media index this job handles. Must be below the media count.
 * @param arenas   Arenas of the worker running the job. Must not be `NULL`.
 * @param log      Log of the job, which receives the failure. Must not be `NULL`.
 * @param userdata `struct ProbeContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording the failure.
 */
static int probe_media_job(ptrdiff_t index,
                           struct JobArenas* arenas,
                           struct Log* log,
                           void* userdata) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Reads a JPEG source and records its EXIF orientation and presented dimensions.
 *
 * The recorded dimensions are the presented ones, so a quarter-turn orientation reports the stored
 * width and height swapped.
 *
 * @param item      Item whose `orientation`, `width_px`, and `height_px` are written on success.
 *                  Must not be `NULL`.
 * @param os        Host the source is read from. Must not be `NULL`.
 * @param scratch   Arena for the source bytes.
 * @param operation Operation leading the failure message, which names the source.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a read failure or invalid JPEG data.
 */
static int probe_image(struct MediaItem* item,
                       struct Os* os,
                       struct Arena scratch,
                       struct Str operation,
                       struct Log* log) __attribute__((nonnull(1, 2, 5)));

/**
 * @brief Probes a video's duration, extracts one frame, and records that frame's dimensions.
 *
 * The frame is taken `video_frame_seconds` into the video, falling back to the midpoint when that
 * position is at or past the end, and to zero for a video of zero duration. The orientation is
 * recorded as top-left because the extracted frame is already auto-rotated. A failure to read the
 * extracted frame still leaves the frame's file for the caller to remove.
 *
 * @param item           Item whose `duration_ms`, `orientation`, `width_px`, and `height_px` are
 *                       written. Must not be `NULL`.
 * @param frame          Frame slot that receives the extracted frame. Must not be `NULL`.
 * @param gallery_config Gallery configuration supplying `video_frame_seconds`. Must not be `NULL`.
 * @param os             Host the frame is read from, which is the real one wherever a video is
 *                       probed. Must not be `NULL`.
 * @param scratch        Arena for the process arguments and the frame bytes.
 * @param operation      Operation leading the failure message, which names the source or the frame.
 * @param log            Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a duration probe, frame extraction, or frame read failure.
 */
static int probe_video(struct MediaItem* item,
                       struct VideoFrame* frame,
                       const struct GalleryConfig* gallery_config,
                       struct Os* os,
                       struct Arena scratch,
                       struct Str operation,
                       struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 7)));

/**
 * @brief Reads the dimensions of an extracted JPEG frame file.
 *
 * @param source_path Video the frame was extracted from, named in every failure.
 * @param frame_path  Path to the extracted frame.
 * @param os          Host the frame is read from. Must not be `NULL`.
 * @param scratch     Arena for the frame bytes.
 * @param width_out   Receives the frame width on success. Must not be `NULL`.
 * @param height_out  Receives the frame height on success. Must not be `NULL`.
 * @param operation   Operation leading the failure message.
 * @param log         Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a read failure or invalid JPEG data.
 */
static int probe_frame(struct Str source_path,
                       struct Str frame_path,
                       struct Os* os,
                       struct Arena scratch,
                       ptrdiff_t* width_out,
                       ptrdiff_t* height_out,
                       struct Str operation,
                       struct Log* log) __attribute__((nonnull(3, 5, 6, 8)));

int album_scanner_build_skeleton(const struct GalleryConfig* gallery_config,
                                 const struct PathList* source_paths,
                                 struct Arena* perm,
                                 struct Arena scratch,
                                 struct Album*** albums_out,
                                 ptrdiff_t* album_count_out,
                                 struct MediaItem*** media_out,
                                 ptrdiff_t* media_count_out,
                                 struct Log* log) {
  struct SkeletonState state = {.perm = *perm, .media_count = source_paths->count};
  state.media = arena_new(&state.perm, state.media_count, struct MediaItem*);
  state.media_albums = arena_new(&scratch, state.media_count, struct Album*);

  // Every source is planned even after one fails, so each path-limit failure in the gallery is
  // reported in one run.
  struct Album* root = NULL;
  int rc = make_album(gallery_config, S(""), NULL, &state.perm, scratch, &root, log);
  insert_album(&state, root);
  const struct Str recipe = gallery_config_derivative_recipe(gallery_config, &scratch);
  const struct Str recipe_root = path_join(S("_fram"), recipe, &scratch);

  for (ptrdiff_t i = 0; i < source_paths->count; i++) {
    const struct Str source_path = source_paths->items[i];
    // The walk lists only files below `input_dir`, so this always finds the relative part.
    struct Str relative = {0};
    (void)path_is_below(source_path, gallery_config->input_dir, &relative);
    const struct Str source_dir = path_parent(relative);
    struct Album* album = NULL;
    if (ensure_source_albums(&state, gallery_config, source_dir, scratch, &album, log) != 0) {
      rc = -1;
    }
    if (add_media(&state, gallery_config, album, i, source_path, relative, recipe_root, scratch,
                  log) != 0) {
      rc = -1;
    }
    state.media_albums[i] = album;
  }
  if (rc != 0) {
    return -1;
  }

  finish_relationships(&state, gallery_config);
  *perm = state.perm;
  *albums_out = state.albums;
  *album_count_out = state.album_count;
  *media_out = state.media;
  *media_count_out = state.media_count;
  return 0;
}

int album_scanner_probe_media(const struct GalleryConfig* gallery_config,
                              struct MediaItem* const* media,
                              ptrdiff_t media_count,
                              struct VideoFrame* video_frames,
                              ptrdiff_t worker_count,
                              struct JobArenas* worker_arenas,
                              bool is_verbose,
                              struct Os* os,
                              struct Arena scratch,
                              struct Log* log) {
  struct ProbeContext context = {
      .gallery_config = gallery_config,
      .media = media,
      .video_frames = video_frames,
      .os = os,
  };
  return job_run(media_count, worker_count, worker_arenas, probe_media_job, &context,
                 S("probing media"), (struct Str){0}, is_verbose, os, scratch, log);
}

void album_scanner_remove_video_frames(struct VideoFrame* video_frames, ptrdiff_t media_count) {
  for (ptrdiff_t i = 0; i < media_count; i++) {
    video_frame_remove(&video_frames[i]);
  }
}

static void insert_album(struct SkeletonState* state, struct Album* album) {
  if (state->album_count == state->album_capacity) {
    state->albums = arena_grow(&state->perm, state->albums, &state->album_capacity,
                               sizeof(*state->albums), _Alignof(struct Album*));
  }
  const ptrdiff_t insert_at =
      find_album_position(state->albums, state->album_count, album->source_dir);
  memmove(state->albums + insert_at + 1, state->albums + insert_at,
          (size_t)(state->album_count - insert_at) * sizeof(*state->albums));
  state->albums[insert_at] = album;
  state->album_count++;
}

static struct Album* find_album(struct Album* const* albums,
                                ptrdiff_t album_count,
                                struct Str source_dir) {
  const ptrdiff_t position = find_album_position(albums, album_count, source_dir);
  if (position < album_count && str_eq(albums[position]->source_dir, source_dir)) {
    return albums[position];
  }
  return NULL;
}

static ptrdiff_t find_album_position(struct Album* const* albums,
                                     ptrdiff_t album_count,
                                     struct Str source_dir) {
  ptrdiff_t low = 0;
  ptrdiff_t high = album_count;
  while (low < high) {
    const ptrdiff_t middle = low + (high - low) / 2;
    if (str_compare(albums[middle]->source_dir, source_dir) < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}

static int make_album(const struct GalleryConfig* gallery_config,
                      struct Str source_dir,
                      struct Album* parent,
                      struct Arena* arena,
                      struct Arena scratch,
                      struct Album** album_out,
                      struct Log* log) {
  struct Str page_relative = S("index.html");
  const struct Str slug_path = make_album_slug_path(source_dir, parent, arena);
  if (slug_path.len > 0) {
    page_relative = path_join(slug_path, S("index.html"), &scratch);
  }
  // A sub-album of a rejected album fails with it, and is not reported a second time.
  const bool is_path_rejected = (parent != NULL && parent->is_path_rejected) ||
                                output_path_check_limits(page_relative, source_dir, log) != 0;
  struct Album* album = arena_new(arena, 1, struct Album);
  album->source_dir = str_clone(source_dir, arena);
  album->slug_path = slug_path;
  album->title = make_album_title(source_dir, gallery_config->title, arena);
  album->depth = path_depth(source_dir);
  album->parent = parent;
  album->path_to_root = output_path_to_root(page_relative, arena);
  album->url_path = str_clone(page_relative, arena);
  album->output_path = path_join(gallery_config->output_dir, page_relative, arena);
  album->is_path_rejected = is_path_rejected;
  *album_out = album;
  return is_path_rejected ? -1 : 0;
}

static int ensure_source_albums(struct SkeletonState* state,
                                const struct GalleryConfig* gallery_config,
                                struct Str source_dir,
                                struct Arena scratch,
                                struct Album** album_out,
                                struct Log* log) {
  int rc = 0;
  struct Album* current = state->albums[0];
  // Each `/` in `source_dir`, and its end, closes one ancestor prefix, which is a view of
  // `source_dir`'s first bytes, so the walk needs no copy. The last prefix is `source_dir` itself.
  for (ptrdiff_t end = 1; end <= source_dir.len; end++) {
    if (end < source_dir.len && source_dir.data[end] != '/') {
      continue;
    }
    const struct Str prefix = str_head(source_dir, end);
    struct Album* next = find_album(state->albums, state->album_count, prefix);
    if (next == NULL) {
      if (make_album(gallery_config, prefix, current, &state->perm, scratch, &next, log) != 0) {
        rc = -1;
      }
      insert_album(state, next);
    }
    current = next;
  }
  *album_out = current;
  return rc;
}

static int add_media(struct SkeletonState* state,
                     const struct GalleryConfig* gallery_config,
                     struct Album* album,
                     ptrdiff_t media_index,
                     struct Str source_path,
                     struct Str source_relative,
                     struct Str recipe_root,
                     struct Arena scratch,
                     struct Log* log) {
  const struct Str file_name = path_basename(source_relative);
  enum MediaKind kind = MEDIA_KIND_IMAGE;
  if (classify_media(file_name, &kind) != 0) {
    return log_error(log, "unsupported media source: '%.*s'", STR_FMT(source_path));
  }
  // An item's outputs extend its album's path, so a rejected album fails the item without another
  // report.
  int rc = album->is_path_rejected ? -1 : 0;
  struct Arena* arena = &state->perm;
  struct MediaItem* item = arena_new(arena, 1, struct MediaItem);
  item->source_path = str_clone(source_path, arena);
  item->title = humanize_underscores(file_name, arena);
  item->kind = kind;
  item->derivative_count = gallery_config->derivative_count;
  item->derivatives = arena_new(arena, item->derivative_count, struct MediaDerivative);

  const struct Str file_slug = text_slugify(file_name, &scratch);
  const struct Str original_extension =
      kind == MEDIA_KIND_VIDEO
          ? S(".mp4")
          : (media_item_has_extension(file_name, S(".jpeg")) ? S(".jpeg") : S(".jpg"));
  const struct Str original_name = str_concat(file_slug, original_extension, &scratch);
  const struct Str original_media_relative = path_join(album->slug_path, original_name, &scratch);
  const struct Str original_relative =
      path_join(S("_fram/originals"), original_media_relative, &scratch);
  // The outputs of one source share its album's path, so an album path past a limit fails each of
  // them. Only the first failure is checked and reported, so a source gives one line, not one per
  // output.
  for (ptrdiff_t i = 0; i < item->derivative_count; i++) {
    struct MediaDerivative* derivative = &item->derivatives[i];
    const struct Str name = gallery_config->derivatives[i].name;
    derivative->name = str_clone(name, arena);
    const struct Str derivative_name = make_derivative_name(file_slug, name, &scratch);
    const struct Str derivative_media_relative =
        path_join(album->slug_path, derivative_name, &scratch);
    const struct Str relative = path_join(recipe_root, derivative_media_relative, &scratch);
    if (rc == 0 && output_path_check_limits(relative, source_path, log) != 0) {
      rc = -1;
    }
    derivative->path = path_join(gallery_config->output_dir, relative, arena);
    derivative->url = str_clone(relative, arena);
  }
  if (rc == 0 && output_path_check_limits(original_relative, source_path, log) != 0) {
    rc = -1;
  }
  item->original_path = path_join(gallery_config->output_dir, original_relative, arena);
  item->original_url = str_clone(original_relative, arena);

  state->media[media_index] = item;
  album->media_count++;
  return rc;
}

static void finish_relationships(struct SkeletonState* state,
                                 const struct GalleryConfig* gallery_config) {
  // Both passes over parents re-find the parent rather than following `child->parent`, which points
  // at the same album. The lookup is what yields a mutable pointer: `Album::parent` is a
  // `const struct Album*`, because a child does not own its parent, and these passes have to write
  // the parent's `sub_album_count` and `sub_albums`. `albums` is the non-const owner, so finding
  // the album there is what makes the write legal. Replacing this with `child->parent` does not
  // compile. Index 1 is the start because `albums[0]` is the root and has no parent.
  for (ptrdiff_t i = 1; i < state->album_count; i++) {
    struct Album* parent =
        find_album(state->albums, state->album_count, state->albums[i]->parent->source_dir);
    parent->sub_album_count++;
  }
  for (ptrdiff_t i = 0; i < state->album_count; i++) {
    struct Album* album = state->albums[i];
    album->media = arena_new(&state->perm, album->media_count, struct MediaItem*);
    album->sub_albums = arena_new(&state->perm, album->sub_album_count, struct Album*);
    // The counts are reset so the two fill loops below can use them as cursors. Their final values
    // are the same numbers the arrays were just sized from.
    album->media_count = 0;
    album->sub_album_count = 0;
  }
  for (ptrdiff_t i = 0; i < state->media_count; i++) {
    struct Album* album = state->media_albums[i];
    album->media[album->media_count++] = state->media[i];
  }
  for (ptrdiff_t i = 1; i < state->album_count; i++) {
    struct Album* child = state->albums[i];
    struct Album* parent = find_album(state->albums, state->album_count, child->parent->source_dir);
    parent->sub_albums[parent->sub_album_count++] = child;
  }
  // Both fill loops above list sources in strictly increasing byte order, so a descending order is
  // their reversal: no two entries tie, and no second sort is needed. Only the per-album arrays are
  // reversed. `find_album` searches `albums`, and the probe and derivative phases index `media`, in
  // source order. This runs before the cover pass, so each cover is the first item in display
  // order.
  for (ptrdiff_t i = 0; i < state->album_count; i++) {
    struct Album* album = state->albums[i];
    if (gallery_config->media_sort_order == GALLERY_SORT_DESC) {
      reverse_media(album->media, album->media_count);
    }
    if (gallery_config->album_sort_order == GALLERY_SORT_DESC) {
      reverse_albums(album->sub_albums, album->sub_album_count);
    }
  }
  // This pass runs from the last album to the first because a parent's total and cover need its
  // children's already computed. Sorting by `source_dir` puts every album ahead of its own
  // descendants, so walking backwards visits every child before its parent.
  for (ptrdiff_t i = state->album_count; i > 0; i--) {
    struct Album* album = state->albums[i - 1];
    album->item_count_total = album->media_count;
    album->cover = album->media_count > 0 ? album->media[0] : NULL;
    for (ptrdiff_t child = 0; child < album->sub_album_count; child++) {
      album->item_count_total += album->sub_albums[child]->item_count_total;
      if (album->cover == NULL) {
        album->cover = album->sub_albums[child]->cover;
      }
    }
  }
}

static void reverse_media(struct MediaItem** media, ptrdiff_t media_count) {
  for (ptrdiff_t i = 0, j = media_count - 1; i < j; i++, j--) {
    struct MediaItem* item = media[i];
    media[i] = media[j];
    media[j] = item;
  }
}

static void reverse_albums(struct Album** albums, ptrdiff_t album_count) {
  for (ptrdiff_t i = 0, j = album_count - 1; i < j; i++, j--) {
    struct Album* album = albums[i];
    albums[i] = albums[j];
    albums[j] = album;
  }
}

static struct Str make_derivative_name(struct Str slug,
                                       struct Str derivative_name,
                                       struct Arena* arena) {
  const struct Str stem = str_concat(slug, S("-"), arena);
  return str_concat(str_concat(stem, derivative_name, arena), S(".jpg"), arena);
}

static struct Str make_album_slug_path(struct Str source_dir,
                                       const struct Album* parent,
                                       struct Arena* arena) {
  if (source_dir.len == 0) {
    return S("");
  }
  const struct Str slug = text_slugify(path_basename(source_dir), arena);
  return parent == NULL || parent->slug_path.len == 0 ? slug
                                                      : path_join(parent->slug_path, slug, arena);
}

static struct Str make_album_title(struct Str source_dir,
                                   struct Str gallery_title,
                                   struct Arena* arena) {
  if (source_dir.len == 0) {
    return gallery_title;
  }
  return humanize_underscores(path_basename(source_dir), arena);
}

static struct Str humanize_underscores(struct Str text, struct Arena* arena) {
  char* title = arena_new(arena, text.len, char);
  for (ptrdiff_t i = 0; i < text.len; i++) {
    title[i] = text.data[i] == '_' ? ' ' : text.data[i];
  }
  return (struct Str){title, text.len};
}

static ptrdiff_t path_depth(struct Str path) {
  if (path.len == 0) {
    return 0;
  }
  ptrdiff_t depth = 1;
  for (ptrdiff_t i = 0; i < path.len; i++) {
    if (path.data[i] == '/') {
      depth++;
    }
  }
  return depth;
}

static int classify_media(struct Str file_name, enum MediaKind* kind_out) {
  if (media_item_has_extension(file_name, S(".jpg")) ||
      media_item_has_extension(file_name, S(".jpeg"))) {
    *kind_out = MEDIA_KIND_IMAGE;
    return 0;
  }
  if (media_item_is_video_path(file_name)) {
    *kind_out = MEDIA_KIND_VIDEO;
    return 0;
  }
  return -1;
}

static int probe_media_job(ptrdiff_t index,
                           struct JobArenas* arenas,
                           struct Log* log,
                           void* userdata) {
  const struct ProbeContext* context = userdata;
  struct MediaItem* item = context->media[index];
  memory_oom_set_job(item->source_path, log);
  const struct Str operation = S("failed to probe media");
  int rc = 0;
  if (item->kind == MEDIA_KIND_IMAGE) {
    rc = probe_image(item, context->os, arenas->scratch, operation, log);
  } else {
    rc = probe_video(item, &context->video_frames[index], context->gallery_config, context->os,
                     arenas->scratch, operation, log);
  }
  if (rc != 0) {
    return -1;
  }
  for (ptrdiff_t i = 0; i < item->derivative_count; i++) {
    const struct GalleryDerivative* config_derivative = &context->gallery_config->derivatives[i];
    struct MediaDerivative* derivative = &item->derivatives[i];
    ptrdiff_t source_width = item->width_px;
    ptrdiff_t source_height = item->height_px;
    if (config_derivative->is_crop) {
      ptrdiff_t crop_x = 0;
      ptrdiff_t crop_y = 0;
      image_center_crop(item->width_px, item->height_px, config_derivative->width_px,
                        config_derivative->height_px, &crop_x, &crop_y, &source_width,
                        &source_height);
    }
    // A crop that would have to enlarge falls back to fitting the cropped region, so a source
    // smaller than the target box yields a smaller derivative rather than an upscaled one.
    if (config_derivative->is_crop && source_width >= config_derivative->width_px &&
        source_height >= config_derivative->height_px) {
      derivative->width_px = config_derivative->width_px;
      derivative->height_px = config_derivative->height_px;
    } else {
      image_fit(source_width, source_height, config_derivative->width_px,
                config_derivative->height_px, &derivative->width_px, &derivative->height_px);
    }
  }
  return 0;
}

static int probe_image(struct MediaItem* item,
                       struct Os* os,
                       struct Arena scratch,
                       struct Str operation,
                       struct Log* log) {
  struct Str data = {0};
  if (os_read(os, item->source_path, IMAGE_INPUT_LEN_MAX, &scratch, &data, operation, log) != 0) {
    return -1;
  }
  struct ImageHeader header;
  const int rc = image_probe(data, &header, operation, item->source_path, log);
  if (rc == 0) {
    item->orientation = exif_read_orientation(data);
    image_orient_size(item->orientation, header.width_px, header.height_px, &item->width_px,
                      &item->height_px);
  }
  // The worker's scratch region outlives the job, so a large source's pages are given back.
  if (data.len >= JOB_SOURCE_DISCARD_LEN_MIN) {
    arena_discard(&scratch, data.data, data.len);
  }
  return rc;
}

static int probe_video(struct MediaItem* item,
                       struct VideoFrame* frame,
                       const struct GalleryConfig* gallery_config,
                       struct Os* os,
                       struct Arena scratch,
                       struct Str operation,
                       struct Log* log) {
  if (video_probe_duration(item->source_path, scratch, &item->duration_ms, operation, log) != 0) {
    return -1;
  }
  // `gallery_config_load` caps `video_frame_seconds` at one day, so the product fits `ptrdiff_t`.
  const ptrdiff_t requested_ms = gallery_config->video_frame_seconds * 1000;
  ptrdiff_t position_ms = requested_ms;
  if (item->duration_ms == 0) {
    position_ms = 0;
  } else if (position_ms >= item->duration_ms) {
    position_ms = item->duration_ms / 2;
  }
  if (video_extract_frame(frame, item->source_path, position_ms, scratch, operation, log) != 0) {
    return -1;
  }
  item->orientation = IMAGE_ORIENTATION_TOP_LEFT;
  return probe_frame(item->source_path, str_from_cstr(frame->path), os, scratch, &item->width_px,
                     &item->height_px, operation, log);
}

static int probe_frame(struct Str source_path,
                       struct Str frame_path,
                       struct Os* os,
                       struct Arena scratch,
                       ptrdiff_t* width_out,
                       ptrdiff_t* height_out,
                       struct Str operation,
                       struct Log* log) {
  // The frame is gone once the build ends, so each failure names the video the frame is for: the
  // read through its operation, and the probe as its file.
  struct Buf frame_operation = {.arena = &scratch};
  buf_printf(&frame_operation, "%.*s: cannot read video frame for '%.*s'", STR_FMT(operation),
             STR_FMT(source_path));
  const struct Str read_operation = buf_str(&frame_operation);
  struct Str data = {0};
  if (os_read(os, frame_path, IMAGE_INPUT_LEN_MAX, &scratch, &data, read_operation, log) != 0) {
    return -1;
  }
  struct ImageHeader header;
  if (image_probe(data, &header, operation, source_path, log) != 0) {
    return -1;
  }
  *width_out = header.width_px;
  *height_out = header.height_px;
  return 0;
}
