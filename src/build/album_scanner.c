#include "build/album_scanner.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "build/job.h"
#include "build/output_path.h"
#include "core/error.h"
#include "core/grow.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/text.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/media_item.h"
#include "formats/exif.h"
#include "formats/image.h"
#include "runtime/fs.h"
#include "runtime/video.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"

enum { ALBUM_CAPACITY_MIN = 8 };

/** Temporary ownership and indexing state while a skeleton is assembled. */
struct SkeletonState {
  /**
   * Albums created so far, kept sorted by `source_dir` for `find_album`'s binary search. Owned here
   * until the scan succeeds and hands the array to the caller.
   */
  struct Album** albums;

  /** Number of albums in `albums`, with the root album at index `0`. */
  size_t album_count;

  /** Allocated slots in `albums`, grown from `ALBUM_CAPACITY_MIN`. */
  size_t album_capacity;

  /**
   * One media item per source path, in source order. Each item lives in its album's arena, so this
   * array carries borrowed pointers and is handed to the caller on success.
   */
  struct MediaItem** media;

  /**
   * Album owning each entry of `media`, at the same index. `finish_relationships` reads it to
   * attach items to albums, and it is freed with the scan rather than handed to the caller.
   */
  struct Album** media_albums;

  /** Number of entries in `media` and `media_albums`, fixed at the source path count. */
  size_t media_count;
};

/**
 * Worker context shared by all probe jobs. Every field is read-only to a job, except
 * `video_frame_paths`, where each job writes its own slot, and `media`, where each job writes its
 * own item's probe fields: `duration_ms`, `orientation`, `width_px`, `height_px`, and each
 * derivative's dimensions.
 */
struct ProbeContext {
  /** Configured derivatives each probed item is sized against. Read-only during the run. */
  const struct GalleryConfig* gallery_config;

  /**
   * Media items to probe, one per job index. The array is read-only during the run. A job writes
   * the probe fields of the single item it owns and allocates in no album arena.
   */
  struct MediaItem* const* media;

  /**
   * One video frame slot per item, receiving the path of the frame extracted from a video. A job
   * writes only `video_frame_paths[index]`, and the caller owns the resulting strings.
   */
  char** video_frame_paths;
};

/**
 * @brief Inserts an album into the skeleton's `source_dir`-sorted album array.
 *
 * Growing the array and choosing the insertion position both happen here, so `albums` stays sorted
 * for `find_album`'s binary search. On failure the array is unchanged and the caller still owns
 * `album`. This records no diagnostic, and the caller reports the allocation failure.
 *
 * @param state Skeleton state owning the album array. Must not be `NULL`.
 * @param album Album whose ownership transfers to `state` on success. Must not be `NULL`.
 * @return `0` on success, or `-1` on capacity overflow or allocation failure.
 */
static int insert_album(struct SkeletonState* state, struct Album* album)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Finds the album whose `source_dir` matches exactly.
 *
 * @param albums      Array sorted by `source_dir`, as `insert_album` maintains. Must not be `NULL`.
 * @param album_count Number of entries in `albums`.
 * @param source_dir  Directory relative to `input_dir` to look for, empty for the root. Must not be
 *                    `NULL`.
 * @return The matching album, or `NULL` when no album carries that `source_dir`.
 */
static struct Album* find_album(struct Album* const* albums,
                                size_t album_count,
                                const char* source_dir) __attribute__((nonnull(1, 3)));

/**
 * @brief Binary-searches for the first album whose `source_dir` does not sort before `source_dir`.
 *
 * Source paths are sorted as whole paths, and `a-b/x.jpg` sorts before `a/y.jpg` while `a` sorts
 * before `a-b` as a directory, so albums are not created in `source_dir` order and each insertion
 * has to search for its place.
 *
 * @param albums      Array sorted by `source_dir`. Must not be `NULL`.
 * @param album_count Number of entries in `albums`.
 * @param source_dir  Directory relative to `input_dir` to place. Must not be `NULL`.
 * @return Index of that album, or `album_count` when every album sorts before `source_dir`.
 */
static size_t find_album_position(struct Album* const* albums,
                                  size_t album_count,
                                  const char* source_dir) __attribute__((nonnull(1, 3)));

/**
 * @brief Allocates one album and derives every path, title, and depth field it owns.
 *
 * The album is separately heap allocated and owns its strings in its own arena, so the caller
 * releases it with `album_free` followed by `free`. The generated page path is checked against the
 * output-path limits here, so an overlong path fails at creation rather than at write time. The
 * album is not linked into `parent`. `finish_relationships` builds the child arrays.
 *
 * @param gallery_config Gallery configuration supplying the gallery title and output directory.
 *                       Must not be `NULL`.
 * @param source_dir     Directory relative to `input_dir`, empty for the root album. Must not be
 *                       `NULL`.
 * @param parent         Album whose `slug_path` prefixes this one, or `NULL` for the root.
 * @param err            Destination for a diagnostic. May be `NULL` only when `err_len` is 0.
 * @param err_len        Size of `err` in bytes.
 * @return The new album, or `NULL` on an output-limit rejection or allocation failure.
 */
static struct Album* make_album(const struct GalleryConfig* gallery_config,
                                const char* source_dir,
                                struct Album* parent,
                                char* err,
                                size_t err_len) __attribute__((nonnull(1, 2)));

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
 * @param source_dir     Directory relative to `input_dir`, empty for the root. Must not be `NULL`.
 * @param err            Destination for a diagnostic. May be `NULL` only when `err_len` is 0.
 * @param err_len        Size of `err` in bytes.
 * @return The album for `source_dir`, owned by `state`, or `NULL` on an output-limit rejection or
 *         allocation failure.
 */
static struct Album* ensure_source_albums(struct SkeletonState* state,
                                          const struct GalleryConfig* gallery_config,
                                          const char* source_dir,
                                          char* err,
                                          size_t err_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Builds one media item and every derivative and original output path it needs.
 *
 * Allocates the item and all of its strings in `album`'s arena, publishes it at
 * `state->media[media_index]`, and counts it on `album`. Only the serial fields are filled. The
 * probe phase writes the dimensions. Each generated output path is checked against the output-path
 * limits here. The item keeps one `MediaDerivative` per configured derivative, in configuration
 * order, so the two arrays can be read at the same index later.
 *
 * @param state           Skeleton state whose flat media array receives the item. Must not be
 *                        `NULL`.
 * @param gallery_config  Gallery configuration supplying the derivatives and output directory. Must
 *                        not be `NULL`.
 * @param album           Album owning the item's storage and counting it. Must not be `NULL`.
 * @param media_index     Slot in `state->media` to publish to. Must be below `state->media_count`.
 * @param source_path     Full source path, stored on the item and named in diagnostics. Must not be
 *                        `NULL`.
 * @param source_relative Same path relative to `input_dir`, whose basename names the item. Must not
 *                        be `NULL`.
 * @param recipe          Derivative recipe component naming the `_fram` output subtree. Must not be
 *                        `NULL`.
 * @param err             Destination for a diagnostic. May be `NULL` only when `err_len` is 0.
 * @param err_len         Size of `err` in bytes.
 * @return `0` on success, or `-1` for an unsupported extension, an output-limit rejection, or
 *         allocation failure.
 */
static int add_media(struct SkeletonState* state,
                     const struct GalleryConfig* gallery_config,
                     struct Album* album,
                     size_t media_index,
                     const char* source_path,
                     const char* source_relative,
                     const char* recipe,
                     char* err,
                     size_t err_len) __attribute__((nonnull(1, 2, 3, 5, 6, 7)));

/**
 * @brief Links albums to their children, attaches media, and computes covers and totals.
 *
 * Runs once every album and media item exists. It counts each album's children, allocates the
 * per-album child and media arrays in that album's own arena, fills them in source order, then
 * accumulates `item_count_total` and `cover` from the deepest album upwards. `state->albums[0]`
 * must be the root, and `state->media_albums[i]` must name the album owning `state->media[i]`.
 *
 * @param state   Skeleton state holding every album and the media-to-album mapping. Must not be
 *                `NULL`.
 * @param err     Destination for a diagnostic. May be `NULL` only when `err_len` is 0.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int finish_relationships(struct SkeletonState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Concatenates a path and a suffix into arena storage.
 *
 * @param path   Terminated prefix. Must not be `NULL`.
 * @param suffix Terminated suffix appended verbatim, including any leading dot. Must not be `NULL`.
 * @param arena  Arena that owns the result. Must not be `NULL`.
 * @return Terminated concatenation owned by `arena`, or `NULL` on allocation failure.
 */
static char* append_suffix(const char* path, const char* suffix, struct Arena* arena)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Builds the `<slug>-<name>.jpg` filename for one derivative.
 *
 * @param slug            Slugified source basename, without its extension. Must not be `NULL`.
 * @param derivative_name Configured derivative name. Must not be `NULL`.
 * @param arena           Arena that owns the result. Must not be `NULL`.
 * @return Terminated filename owned by `arena`, or `NULL` on allocation failure.
 */
static char* make_derivative_name(const char* slug,
                                  const char* derivative_name,
                                  struct Arena* arena) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Builds an album's URL-safe output directory from its basename under its parent.
 *
 * Only the last component is slugified. The ancestors come from `parent->slug_path`, which was
 * built the same way, so each component is slugified exactly once.
 *
 * @param source_dir Directory relative to `input_dir`, empty for the root. Must not be `NULL`.
 * @param parent     Album supplying the prefix, or `NULL` for a top-level album.
 * @param arena      Arena that owns the result. Must not be `NULL`.
 * @return Terminated slug path owned by `arena`, empty for the root, or `NULL` on overflow or
 *         allocation failure.
 */
static char* make_album_slug_path(const char* source_dir,
                                  const struct Album* parent,
                                  struct Arena* arena) __attribute__((nonnull(1, 3)));

/**
 * @brief Derives an album's display title from its directory basename.
 *
 * @param source_dir    Directory relative to `input_dir`, empty for the root. Must not be `NULL`.
 * @param gallery_title Title used verbatim for the root album. Must not be `NULL`.
 * @param arena         Arena that owns the result. Must not be `NULL`.
 * @return Terminated title owned by `arena`, or `NULL` on allocation failure.
 */
static char* make_album_title(const char* source_dir,
                              const char* gallery_title,
                              struct Arena* arena) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Copies text with every underscore replaced by a space.
 *
 * @param text  Terminated source text. Must not be `NULL`.
 * @param arena Arena that owns the copy. Must not be `NULL`.
 * @return Terminated copy owned by `arena`, or `NULL` on allocation failure.
 */
static char* humanize_underscores(const char* text, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Counts the directory edges a relative path lies below the root.
 *
 * @param path Relative directory path with `/` separators. Must not be `NULL`.
 * @return `0` for an empty path, otherwise one more than its separator count.
 */
static size_t path_depth(const char* path) __attribute__((nonnull(1)));

/**
 * @brief Classifies a media source by its filename extension, folding ASCII case.
 *
 * @param file_name Bare source filename. Must not be `NULL`.
 * @param is_valid  Receives whether the extension is one this program supports. Must not be `NULL`.
 * @return `MEDIA_KIND_IMAGE` for `.jpg` or `.jpeg` and `MEDIA_KIND_VIDEO` for `.mp4`.
 *         `MEDIA_KIND_IMAGE` is also returned for an unsupported extension, so a caller must
 *         consult `*is_valid` before using the kind.
 */
static enum MediaKind classify_media(const char* file_name, bool* is_valid)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Probes one media item's source dimensions and sizes each of its derivatives.
 *
 * This is the `JobFn` of the probe phase, so it runs concurrently with other indexes and writes
 * only the slots belonging to `index`: the item's scalar probe fields, its derivative dimensions,
 * and, for a video, `video_frame_paths[index]`. It allocates nothing in an album arena. A failure
 * is latched in the job set rather than reported through a buffer.
 *
 * @param jobs     Running job set, whose error slot for `index` the job may fill through
 *                 `job_set_error`. Must not be `NULL`.
 * @param index    Media index this job handles. Must be below the media count.
 * @param userdata `struct ProbeContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording the failure through `job_set_error`.
 */
static int probe_media_job(struct JobSet* jobs, size_t index, void* userdata)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Reads a JPEG source and records its EXIF orientation and presented dimensions.
 *
 * The recorded dimensions are the presented ones, so a quarter-turn orientation reports the stored
 * width and height swapped.
 *
 * @param item       Item whose `orientation`, `width_px`, and `height_px` are written on success.
 *                   Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a read failure or invalid JPEG data.
 */
static int probe_image(struct MediaItem* item, char* reason, size_t reason_len)
    __attribute__((nonnull(1)));

/**
 * @brief Probes a video's duration, extracts one frame, and records that frame's dimensions.
 *
 * The frame is taken `video_frame_seconds` into the video, falling back to the midpoint when that
 * position is at or past the end, and to zero for a video of zero duration. The orientation is
 * recorded as top-left because the extracted frame is already auto-rotated. A failure to read the
 * extracted frame still leaves an owned path in `*frame_path_out`.
 *
 * @param gallery_config Gallery configuration supplying `video_frame_seconds`. Must not be `NULL`.
 * @param item           Item whose `duration_ms`, `orientation`, `width_px`, and `height_px` are
 *                       written. Must not be `NULL`.
 * @param frame_path_out Receives the malloc-owned temporary JPEG path, which the caller must unlink
 *                       and free. Must not be `NULL`.
 * @param reason         Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len     Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a duration probe, frame extraction, or frame read failure.
 */
static int probe_video(const struct GalleryConfig* gallery_config,
                       struct MediaItem* item,
                       char** frame_path_out,
                       char* reason,
                       size_t reason_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Reads the dimensions of an extracted JPEG frame file.
 *
 * @param frame_path Path to the extracted frame. Must not be `NULL`.
 * @param width_out  Receives the frame width on success. Must not be `NULL`.
 * @param height_out Receives the frame height on success. Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a read failure or invalid JPEG data.
 */
static int probe_frame(const char* frame_path,
                       size_t* width_out,
                       size_t* height_out,
                       char* reason,
                       size_t reason_len) __attribute__((nonnull(1, 2, 3)));

int album_scanner_build_skeleton(const struct GalleryConfig* gallery_config,
                                 const struct PathList* source_paths,
                                 struct Album*** albums_out,
                                 size_t* album_count_out,
                                 struct MediaItem*** media_out,
                                 size_t* media_count_out,
                                 char* err,
                                 size_t err_len) {
  *albums_out = NULL;
  *album_count_out = 0;
  *media_out = NULL;
  *media_count_out = 0;

  struct SkeletonState state = {0};
  struct Album* root = NULL;
  char* recipe = NULL;
  state.media_count = source_paths->count;
  if (state.media_count > 0) {
    state.media = calloc(state.media_count, sizeof(*state.media));
    state.media_albums = calloc(state.media_count, sizeof(*state.media_albums));
    if (state.media == NULL || state.media_albums == NULL) {
      (void)error_report(err, err_len, "out of memory building album skeleton");
      goto fail;
    }
  }

  root = make_album(gallery_config, "", NULL, err, err_len);
  if (root == NULL) {
    goto fail;
  }
  if (insert_album(&state, root) != 0) {
    album_free(root);
    free(root);
    (void)error_report(err, err_len, "out of memory building album skeleton");
    goto fail;
  }
  recipe = gallery_config_derivative_recipe(gallery_config, &root->arena);
  if (recipe == NULL) {
    (void)error_report(err, err_len, "out of memory building derivative recipe");
    goto fail;
  }

  for (size_t i = 0; i < source_paths->count; i++) {
    const char* source_path = source_paths->items[i];
    if (i > 0 && strcmp(source_paths->items[i - 1], source_path) >= 0) {
      (void)error_report(err, err_len, "source paths are not strictly sorted: '%s'", source_path);
      goto fail;
    }
    const char* relative = path_relative_below(source_path, gallery_config->input_dir);
    if (relative == NULL || *relative == '\0') {
      (void)error_report(
          err, err_len, "media source must be under the configured 'input_dir': '%s'", source_path);
      goto fail;
    }
    const char* slash = strrchr(relative, '/');
    const size_t source_dir_len = slash == NULL ? 0 : (size_t)(slash - relative);
    char* source_dir = malloc(source_dir_len + 1);
    if (source_dir == NULL) {
      (void)error_report(err, err_len, "out of memory building album skeleton");
      goto fail;
    }
    memcpy(source_dir, relative, source_dir_len);
    source_dir[source_dir_len] = '\0';
    struct Album* album = ensure_source_albums(&state, gallery_config, source_dir, err, err_len);
    free(source_dir);
    if (album == NULL) {
      goto fail;
    }
    if (add_media(&state, gallery_config, album, i, source_path, relative, recipe, err, err_len) !=
        0) {
      goto fail;
    }
    state.media_albums[i] = album;
  }

  if (finish_relationships(&state, err, err_len) != 0) {
    goto fail;
  }
  free(state.media_albums);
  *albums_out = state.albums;
  *album_count_out = state.album_count;
  *media_out = state.media;
  *media_count_out = state.media_count;
  return 0;

fail:
  free(state.media_albums);
  album_scanner_free_skeleton(state.albums, state.album_count, state.media);
  return -1;
}

void album_scanner_free_skeleton(struct Album** albums,
                                 size_t album_count,
                                 struct MediaItem** media) {
  for (size_t i = 0; i < album_count; i++) {
    album_free(albums[i]);
    free(albums[i]);
  }
  free(albums);
  free(media);
}

int album_scanner_probe_media(const struct GalleryConfig* gallery_config,
                              struct MediaItem* const* media,
                              size_t media_count,
                              char** video_frame_paths,
                              size_t worker_count,
                              bool is_verbose,
                              struct StringBuffer* error_out) {
  struct ProbeContext context = {
      .gallery_config = gallery_config,
      .media = media,
      .video_frame_paths = video_frame_paths,
  };
  return job_run(media_count, worker_count, probe_media_job, &context, "probing media", is_verbose,
                 error_out);
}

void album_scanner_free_video_frames(char** video_frame_paths, size_t media_count) {
  if (video_frame_paths == NULL) {
    return;
  }
  for (size_t i = 0; i < media_count; i++) {
    if (video_frame_paths[i] != NULL) {
      (void)unlink(video_frame_paths[i]);
      free(video_frame_paths[i]);
      video_frame_paths[i] = NULL;
    }
  }
}

static int insert_album(struct SkeletonState* state, struct Album* album) {
  if (state->album_count == state->album_capacity) {
    size_t capacity_next = 0;
    size_t capacity_bytes = 0;
    if (grow_capacity(state->album_capacity, ALBUM_CAPACITY_MIN, sizeof(*state->albums),
                      &capacity_next, &capacity_bytes) != 0) {
      return -1;
    }
    struct Album** albums = realloc(state->albums, capacity_bytes);
    if (albums == NULL) {
      return -1;
    }
    state->albums = albums;
    state->album_capacity = capacity_next;
  }
  const size_t insert_at =
      find_album_position(state->albums, state->album_count, album->source_dir);
  memmove(state->albums + insert_at + 1, state->albums + insert_at,
          (state->album_count - insert_at) * sizeof(*state->albums));
  state->albums[insert_at] = album;
  state->album_count++;
  return 0;
}

static struct Album* find_album(struct Album* const* albums,
                                size_t album_count,
                                const char* source_dir) {
  const size_t position = find_album_position(albums, album_count, source_dir);
  if (position < album_count && strcmp(albums[position]->source_dir, source_dir) == 0) {
    return albums[position];
  }
  return NULL;
}

static size_t find_album_position(struct Album* const* albums,
                                  size_t album_count,
                                  const char* source_dir) {
  size_t low = 0;
  size_t high = album_count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (strcmp(albums[middle]->source_dir, source_dir) < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}

static struct Album* make_album(const struct GalleryConfig* gallery_config,
                                const char* source_dir,
                                struct Album* parent,
                                char* err,
                                size_t err_len) {
  struct Album* album = calloc(1, sizeof(*album));
  if (album == NULL) {
    (void)error_report(err, err_len, "out of memory building album");
    return NULL;
  }
  album->source_dir = arena_strdup(&album->arena, source_dir);
  album->slug_path = make_album_slug_path(source_dir, parent, &album->arena);
  album->title = make_album_title(source_dir, gallery_config->title, &album->arena);
  album->depth = path_depth(source_dir);
  album->parent = parent;
  if (album->source_dir == NULL || album->slug_path == NULL || album->title == NULL) {
    (void)error_report(err, err_len, "out of memory building album");
    album_free(album);
    free(album);
    return NULL;
  }

  const char* page_relative = "index.html";
  if (*album->slug_path != '\0') {
    page_relative = path_join(album->slug_path, "index.html", &album->arena);
  }
  if (page_relative == NULL) {
    (void)error_report(err, err_len, "out of memory building album output path");
    album_free(album);
    free(album);
    return NULL;
  }
  if (output_path_check_limits(page_relative, source_dir, err, err_len) != 0) {
    album_free(album);
    free(album);
    return NULL;
  }
  album->path_to_root = output_path_to_root(page_relative, &album->arena);
  album->url_path = media_item_encode_url(page_relative, &album->arena);
  album->output_path = path_join(gallery_config->output_dir, page_relative, &album->arena);
  if (album->path_to_root == NULL || album->url_path == NULL || album->output_path == NULL) {
    (void)error_report(err, err_len, "out of memory building album output path");
    album_free(album);
    free(album);
    return NULL;
  }
  return album;
}

static struct Album* ensure_source_albums(struct SkeletonState* state,
                                          const struct GalleryConfig* gallery_config,
                                          const char* source_dir,
                                          char* err,
                                          size_t err_len) {
  struct Album* current = state->albums[0];
  if (*source_dir == '\0') {
    return current;
  }
  const size_t source_dir_len = strlen(source_dir);
  char* prefixes = malloc(source_dir_len + 1);
  if (prefixes == NULL) {
    (void)error_report(err, err_len, "out of memory building album tree");
    return NULL;
  }
  memcpy(prefixes, source_dir, source_dir_len + 1);
  // `prefixes` is walked in place: each separator is temporarily overwritten with a terminator so
  // the buffer spells one ancestor prefix at a time, then restored before moving on. The loop ends
  // on the component that was already terminated, which is `source_dir` itself.
  for (char* p = prefixes;; p++) {
    if (*p != '/' && *p != '\0') {
      continue;
    }
    const char saved = *p;
    *p = '\0';
    struct Album* next = find_album(state->albums, state->album_count, prefixes);
    if (next == NULL) {
      next = make_album(gallery_config, prefixes, current, err, err_len);
      if (next == NULL) {
        free(prefixes);
        return NULL;
      }
      if (insert_album(state, next) != 0) {
        album_free(next);
        free(next);
        free(prefixes);
        (void)error_report(err, err_len, "out of memory building album tree");
        return NULL;
      }
    }
    current = next;
    *p = saved;
    if (saved == '\0') {
      break;
    }
  }
  free(prefixes);
  return current;
}

static int add_media(struct SkeletonState* state,
                     const struct GalleryConfig* gallery_config,
                     struct Album* album,
                     size_t media_index,
                     const char* source_path,
                     const char* source_relative,
                     const char* recipe,
                     char* err,
                     size_t err_len) {
  const char* file_name = strrchr(source_relative, '/');
  file_name = file_name == NULL ? source_relative : file_name + 1;
  bool is_valid = false;
  const enum MediaKind kind = classify_media(file_name, &is_valid);
  if (!is_valid) {
    return error_report(err, err_len, "unsupported media source: '%s'", source_path);
  }
  struct MediaItem* item = arena_calloc(&album->arena, 1, sizeof(*item));
  if (item == NULL) {
    return error_report(err, err_len, "out of memory building media item");
  }
  item->source_path = arena_strdup(&album->arena, source_path);
  item->title = humanize_underscores(file_name, &album->arena);
  item->kind = kind;
  item->derivative_count = gallery_config->derivative_count;
  item->derivatives =
      arena_calloc(&album->arena, item->derivative_count, sizeof(*item->derivatives));

  size_t file_slug_len = 0;
  char* file_slug = text_slugify(file_name, strlen(file_name), &album->arena, &file_slug_len);
  (void)file_slug_len;
  const char* original_extension =
      kind == MEDIA_KIND_VIDEO ? ".mp4"
                               : (media_item_has_extension(file_name, ".jpeg") ? ".jpeg" : ".jpg");
  char* original_name =
      file_slug == NULL ? NULL : append_suffix(file_slug, original_extension, &album->arena);
  char* original_media_relative =
      original_name == NULL ? NULL : path_join(album->slug_path, original_name, &album->arena);

  char* recipe_root = path_join("_fram", recipe, &album->arena);
  char* original_root = path_join("_fram", "originals", &album->arena);
  char* original_relative = original_root == NULL || original_media_relative == NULL
                                ? NULL
                                : path_join(original_root, original_media_relative, &album->arena);
  if (item->source_path == NULL || item->title == NULL || item->derivatives == NULL ||
      file_slug == NULL || recipe_root == NULL || original_relative == NULL) {
    return error_report(err, err_len, "out of memory building media paths");
  }
  for (size_t i = 0; i < item->derivative_count; i++) {
    struct MediaDerivative* derivative = &item->derivatives[i];
    derivative->name = arena_strdup(&album->arena, gallery_config->derivatives[i].name);
    char* derivative_name =
        make_derivative_name(file_slug, gallery_config->derivatives[i].name, &album->arena);
    char* derivative_media_relative =
        derivative_name == NULL ? NULL
                                : path_join(album->slug_path, derivative_name, &album->arena);
    char* relative = derivative_media_relative == NULL
                         ? NULL
                         : path_join(recipe_root, derivative_media_relative, &album->arena);
    if (derivative->name == NULL || relative == NULL ||
        output_path_check_limits(relative, source_path, err, err_len) != 0) {
      return relative == NULL ? error_report(err, err_len, "out of memory building media paths")
                              : -1;
    }
    derivative->path = path_join(gallery_config->output_dir, relative, &album->arena);
    derivative->url = media_item_encode_url(relative, &album->arena);
    if (derivative->path == NULL || derivative->url == NULL) {
      return error_report(err, err_len, "out of memory building media paths");
    }
  }
  if (output_path_check_limits(original_relative, source_path, err, err_len) != 0) {
    return -1;
  }
  item->original_path = path_join(gallery_config->output_dir, original_relative, &album->arena);
  item->original_url = media_item_encode_url(original_relative, &album->arena);
  if (item->original_path == NULL || item->original_url == NULL) {
    return error_report(err, err_len, "out of memory building media paths");
  }

  state->media[media_index] = item;
  album->media_count++;
  return 0;
}

static int finish_relationships(struct SkeletonState* state, char* err, size_t err_len) {
  // Both passes over parents re-find the parent rather than following `child->parent`, which points
  // at the same album. The lookup is what yields a mutable pointer: `Album::parent` is a
  // `const struct Album*`, because a child does not own its parent, and these passes have to write
  // the parent's `sub_album_count` and `sub_albums`. `albums` is the non-const owner, so finding
  // the album there is what makes the write legal. Replacing this with `child->parent` does not
  // compile. Index 1 is the start because `albums[0]` is the root and has no parent.
  for (size_t i = 1; i < state->album_count; i++) {
    struct Album* parent =
        find_album(state->albums, state->album_count, state->albums[i]->parent->source_dir);
    parent->sub_album_count++;
  }
  for (size_t i = 0; i < state->album_count; i++) {
    struct Album* album = state->albums[i];
    const size_t media_count = album->media_count;
    const size_t sub_album_count = album->sub_album_count;
    album->media = arena_calloc(&album->arena, media_count, sizeof(*album->media));
    album->sub_albums = arena_calloc(&album->arena, sub_album_count, sizeof(*album->sub_albums));
    if (album->media == NULL || album->sub_albums == NULL) {
      return error_report(err, err_len, "out of memory finishing album tree");
    }
    // The counts are reset so the two fill loops below can use them as cursors. Their final values
    // are the same numbers the arrays were just sized from.
    album->media_count = 0;
    album->sub_album_count = 0;
  }
  for (size_t i = 0; i < state->media_count; i++) {
    struct Album* album = state->media_albums[i];
    album->media[album->media_count++] = state->media[i];
  }
  for (size_t i = 1; i < state->album_count; i++) {
    struct Album* child = state->albums[i];
    struct Album* parent = find_album(state->albums, state->album_count, child->parent->source_dir);
    parent->sub_albums[parent->sub_album_count++] = child;
  }
  // This pass runs from the last album to the first because a parent's total and cover need its
  // children's already computed. Sorting by `source_dir` puts every album ahead of its own
  // descendants, so walking backwards visits every child before its parent.
  for (size_t i = state->album_count; i > 0; i--) {
    struct Album* album = state->albums[i - 1];
    album->item_count_total = album->media_count;
    album->cover = album->media_count > 0 ? album->media[0] : NULL;
    for (size_t child = 0; child < album->sub_album_count; child++) {
      album->item_count_total += album->sub_albums[child]->item_count_total;
      if (album->cover == NULL) {
        album->cover = album->sub_albums[child]->cover;
      }
    }
  }
  return 0;
}

static char* append_suffix(const char* path, const char* suffix, struct Arena* arena) {
  const size_t path_len = strlen(path);
  const size_t suffix_len = strlen(suffix);
  char* result = arena_alloc(arena, path_len + suffix_len + 1);
  if (result == NULL) {
    return NULL;
  }
  memcpy(result, path, path_len);
  memcpy(result + path_len, suffix, suffix_len + 1);
  return result;
}

static char* make_derivative_name(const char* slug,
                                  const char* derivative_name,
                                  struct Arena* arena) {
  const size_t slug_len = strlen(slug);
  const size_t derivative_len = strlen(derivative_name);
  static const char extension[] = ".jpg";
  const size_t name_len = slug_len + 1 + derivative_len + sizeof(extension) - 1;
  char* name = arena_alloc(arena, name_len + 1);
  if (name == NULL) {
    return NULL;
  }
  (void)snprintf(name, name_len + 1, "%s-%s%s", slug, derivative_name, extension);
  return name;
}

static char* make_album_slug_path(const char* source_dir,
                                  const struct Album* parent,
                                  struct Arena* arena) {
  if (*source_dir == '\0') {
    return arena_strdup(arena, "");
  }
  const char* base = strrchr(source_dir, '/');
  base = base == NULL ? source_dir : base + 1;
  size_t slug_len = 0;
  char* slug = text_slugify(base, strlen(base), arena, &slug_len);
  if (slug == NULL) {
    return NULL;
  }
  (void)slug_len;
  return parent == NULL || parent->slug_path[0] == '\0' ? slug
                                                        : path_join(parent->slug_path, slug, arena);
}

static char* make_album_title(const char* source_dir,
                              const char* gallery_title,
                              struct Arena* arena) {
  if (*source_dir == '\0') {
    return arena_strdup(arena, gallery_title);
  }
  const char* base = strrchr(source_dir, '/');
  return humanize_underscores(base == NULL ? source_dir : base + 1, arena);
}

static char* humanize_underscores(const char* text, struct Arena* arena) {
  char* title = arena_strdup(arena, text);
  if (title == NULL) {
    return NULL;
  }
  for (char* p = title; *p != '\0'; p++) {
    if (*p == '_') {
      *p = ' ';
    }
  }
  return title;
}

static size_t path_depth(const char* path) {
  if (*path == '\0') {
    return 0;
  }
  size_t depth = 1;
  for (const char* p = path; *p != '\0'; p++) {
    if (*p == '/') {
      depth++;
    }
  }
  return depth;
}

static enum MediaKind classify_media(const char* file_name, bool* is_valid) {
  if (media_item_has_extension(file_name, ".jpg") || media_item_has_extension(file_name, ".jpeg")) {
    *is_valid = true;
    return MEDIA_KIND_IMAGE;
  }
  if (media_item_is_video_path(file_name)) {
    *is_valid = true;
    return MEDIA_KIND_VIDEO;
  }
  *is_valid = false;
  return MEDIA_KIND_IMAGE;
}

static int probe_media_job(struct JobSet* jobs, size_t index, void* userdata) {
  const struct ProbeContext* context = userdata;
  struct MediaItem* item = context->media[index];
  char reason[FS_REASON_SIZE];
  int rc = 0;
  if (item->kind == MEDIA_KIND_IMAGE) {
    rc = probe_image(item, reason, sizeof(reason));
  } else {
    rc = probe_video(context->gallery_config, item, &context->video_frame_paths[index], reason,
                     sizeof(reason));
  }
  if (rc != 0) {
    job_set_error(jobs, index, "failed to probe media: %s ('%s')", reason, item->source_path);
    return -1;
  }
  for (size_t i = 0; i < item->derivative_count; i++) {
    const struct GalleryDerivative* config_derivative = &context->gallery_config->derivatives[i];
    struct MediaDerivative* derivative = &item->derivatives[i];
    size_t source_width = item->width_px;
    size_t source_height = item->height_px;
    if (config_derivative->is_crop) {
      size_t crop_x = 0;
      size_t crop_y = 0;
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

static int probe_image(struct MediaItem* item, char* reason, size_t reason_len) {
  unsigned char* data = NULL;
  size_t data_len = 0;
  if (fs_read_file(item->source_path, IMAGE_INPUT_LEN_MAX, &data, &data_len, reason, reason_len) !=
      0) {
    return -1;
  }
  size_t width = 0;
  size_t height = 0;
  const int rc = image_probe(data, data_len, &width, &height, reason, reason_len);
  if (rc == 0) {
    item->orientation = exif_read_orientation(data, data_len);
    image_orient_size(item->orientation, width, height, &item->width_px, &item->height_px);
  }
  free(data);
  return rc;
}

static int probe_video(const struct GalleryConfig* gallery_config,
                       struct MediaItem* item,
                       char** frame_path_out,
                       char* reason,
                       size_t reason_len) {
  if (video_probe_duration(item->source_path, &item->duration_ms, reason, reason_len) != 0) {
    return -1;
  }
  // `gallery_config_load` caps `video_frame_seconds` at one day, so the product fits `size_t`.
  const size_t requested_ms = gallery_config->video_frame_seconds * 1000;
  size_t position_ms = requested_ms;
  if (item->duration_ms == 0) {
    position_ms = 0;
  } else if (position_ms >= item->duration_ms) {
    position_ms = item->duration_ms / 2;
  }
  if (video_extract_frame(item->source_path, position_ms, frame_path_out, reason, reason_len) !=
      0) {
    return -1;
  }
  item->orientation = IMAGE_ORIENTATION_TOP_LEFT;
  return probe_frame(*frame_path_out, &item->width_px, &item->height_px, reason, reason_len);
}

static int probe_frame(const char* frame_path,
                       size_t* width_out,
                       size_t* height_out,
                       char* reason,
                       size_t reason_len) {
  unsigned char* data = NULL;
  size_t data_len = 0;
  if (fs_read_file(frame_path, IMAGE_INPUT_LEN_MAX, &data, &data_len, reason, reason_len) != 0) {
    return -1;
  }
  const int rc = image_probe(data, data_len, width_out, height_out, reason, reason_len);
  free(data);
  return rc;
}
