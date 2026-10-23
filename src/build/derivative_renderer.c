#include "build/derivative_renderer.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "build/job.h"
#include "core/error.h"
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

/**
 * Bytes the largest decode can hold, as `image_decode_len` counts it: `IMAGE_PIXEL_COUNT_MAX`
 * pixels at full size, three bytes each, beside the coefficients of a progressive CMYK JPEG, whose
 * four components are all at full resolution, eight bytes per pixel. Padding each component's
 * blocks to whole sampling units adds at most 32 pixels to each dimension, under 1% within the
 * image limits, which the extra 1% covers.
 */
#define DECODE_LEN_MAX ((ptrdiff_t)IMAGE_PIXEL_COUNT_MAX * (IMAGE_CHANNEL_COUNT + 8) / 100 * 101)

/**
 * Largest total of decode and derivative-write memory, in bytes, that derivative jobs hold at once,
 * across every worker.
 *
 * A job holds this memory from the start of its decode to the end of its last derivative write,
 * and it grows with the dimensions the source declares rather than with its file size: a JPEG of a
 * few hundred bytes can declare `IMAGE_PIXEL_COUNT_MAX` pixels. One job per worker would multiply
 * that by the core count. At least two of the largest decodes fit, whatever the worker count, and
 * smaller sources share the budget accordingly. The decoded pixels decision in `ARCHITECTURE.md`
 * gives the measured peaks.
 */
#define DECODE_BUDGET_LEN (2 * DECODE_LEN_MAX)

/**
 * How many times each dimension of a derivative its source region is, at least, after a scaled
 * decode. Reducing straight to a derivative's size leaves the DCT's box-like reduction all of the
 * work, which visibly softens it.
 */
enum { DECODE_SCALE_HEADROOM = 2 };

/** Decode-memory reservations shared by every derivative worker in the process. */
struct DecodeBudget {
  /** Guards `len_reserved`. */
  pthread_mutex_t mutex;

  /** Signalled whenever a job returns its reservation. */
  pthread_cond_t released;

  /** Bytes currently reserved by jobs holding a decode, at most `DECODE_BUDGET_LEN`. */
  ptrdiff_t len_reserved;
};

static struct DecodeBudget decode_budget = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .released = PTHREAD_COND_INITIALIZER,
    .len_reserved = 0,
};

/** Worker context shared by all derivative jobs. Every field is read-only to a job. */
struct DerivativeContext {
  /** Configured derivative sizes, crops, and qualities. Read-only during the run. */
  const struct GalleryConfig* gallery_config;

  /**
   * Media items to render, one per job index. Read-only during the run: this phase reads the paths
   * and dimensions the probe recorded and writes nothing back into an item.
   */
  struct MediaItem* const* media;

  /**
   * Extracted video frames, one slot per job index. A job reads its own frame and removes it, which
   * clears the slot's `is_created`, so the job writes only its own slot.
   */
  struct VideoFrame* video_frames;

  /** Host every job reads its source from and writes its outputs to. */
  struct Os* os;
};

/**
 * @brief Generates one media item's stale derivatives and copies its original when out of date.
 *
 * This is the `JobFn` of the derivative phase, so it runs concurrently with other indexes and
 * writes only the outputs belonging to `index` and its own frame slot. A video's extracted frame is
 * removed before returning on every path. Everything the job allocates goes into a copy of its
 * worker's scratch, except the decode's memory, which stays on `malloc` under the decode budget.
 *
 * @param index    Media index this job handles. Must be below the media count.
 * @param arenas   Arenas of the worker running the job. Must not be `NULL`.
 * @param log      Log of the job, which receives every failure. Must not be `NULL`.
 * @param userdata `struct DerivativeContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a failure.
 */
static int render_derivatives_job(ptrdiff_t index,
                                  struct JobArenas* arenas,
                                  struct Log* log,
                                  void* userdata) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Writes one media item's stale derivatives and its stale original copy.
 *
 * An output already at least as new as the source is skipped, and the source is read, decoded, and
 * oriented at most once for the whole item. Each failure is logged and the job carries on, so a
 * failing derivative does not hide a failing original copy.
 *
 * @param context Shared derivative-phase state. Must not be `NULL`.
 * @param index   Media index to render.
 * @param scratch Arena for the source bytes.
 * @param log     Log of the job, which receives every failure. Must not be `NULL`.
 * @return `0` when nothing failed, or `-1` after recording at least one failure.
 */
static int render_derivatives_outputs(const struct DerivativeContext* context,
                                      ptrdiff_t index,
                                      struct Arena scratch,
                                      struct Log* log) __attribute__((nonnull(1, 4)));

/**
 * @brief Inspects one media source before its outputs are compared against it.
 *
 * A source that became a directory after the walk is reported here, once, rather than by both the
 * decode and the copy of the original.
 *
 * @param source_path Media source to inspect.
 * @param os          Host the source is on. Must not be `NULL`.
 * @param stat_out    Receives the source's metadata. Must not be `NULL`.
 * @param log         Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` when the source is a file, or `-1` after recording why it cannot be used.
 */
static int render_derivatives_outputs_inspect(struct Str source_path,
                                              struct Os* os,
                                              struct OsStat* stat_out,
                                              struct Log* log) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Reads, decodes, and orients one item's source, then writes each of its stale derivatives.
 *
 * The source is decoded at the smallest scale its stale derivatives allow. The job runs under a
 * `decode_budget` reservation for the most it holds at once: while it decodes and orients the
 * source, as `image_decode_len` counts it, or while it writes its largest stale derivative beside
 * the oriented source. The reservation is kept until the decoded image is released.
 *
 * @param context  Shared derivative-phase state. Must not be `NULL`.
 * @param index    Media index whose derivatives are written.
 * @param is_stale One flag per derivative of the item, `true` where the output must be rewritten.
 *                 Must not be `NULL`.
 * @param scratch  Arena for the source bytes.
 * @param log      Log of the job, which receives every failure. Must not be `NULL`.
 * @return `0` when every stale derivative was written, or `-1` after recording a failure.
 */
static int render_derivatives_images(const struct DerivativeContext* context,
                                     ptrdiff_t index,
                                     const bool* is_stale,
                                     struct Arena scratch,
                                     struct Log* log) __attribute__((nonnull(1, 3, 5)));

/**
 * @brief Decodes a JPEG source at a scale and applies its orientation.
 *
 * Once decoded, the pixels hold all the derivatives need, so a source of
 * `JOB_SOURCE_DISCARD_LEN_MIN` or more gives its pages back before `image_orient`, which may hold
 * two buffers of the decoded size.
 *
 * @param data                   JPEG bytes of the source image or video frame.
 * @param scale_eighths          Decode scale, from 1 through `IMAGE_SCALE_FULL`.
 * @param orientation            Orientation to apply to the decoded pixels.
 * @param scratch                Arena `data` was read into. Must not be `NULL`.
 * @param image_out              Receives the oriented image, which the caller releases with
 *                               `image_free` on success and failure alike. Must not be `NULL`.
 * @param derivative_source_path Path the JPEG bytes were read from, for diagnostics.
 * @param log                    Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a decode failure.
 */
static int decode_oriented(struct Str data,
                           ptrdiff_t scale_eighths,
                           enum ImageOrientation orientation,
                           const struct Arena* scratch,
                           struct Image* image_out,
                           struct Str derivative_source_path,
                           struct Log* log) __attribute__((nonnull(4, 5, 7)));

/**
 * @brief Writes every stale derivative of one item from its oriented source.
 *
 * Each failed derivative is recorded and the loop carries on, so one failure does not hide another.
 *
 * @param context  Shared derivative-phase state. Must not be `NULL`.
 * @param index    Media index whose derivatives are written.
 * @param is_stale One flag per derivative of the item, `true` where the output must be rewritten.
 *                 Must not be `NULL`.
 * @param oriented Oriented source image, at its decoded scale. Must not be `NULL`.
 * @param scratch  Arena for each failure message's operation.
 * @param log      Log of the job, which receives every failure. Must not be `NULL`.
 * @return `0` when every stale derivative was written, or `-1` after recording a failure.
 */
static int write_stale_derivatives(const struct DerivativeContext* context,
                                   ptrdiff_t index,
                                   const bool* is_stale,
                                   const struct Image* oriented,
                                   struct Arena scratch,
                                   struct Log* log) __attribute__((nonnull(1, 3, 4, 6)));

/**
 * @brief Selects the part of a source that a derivative is resized from.
 *
 * That is the whole source, or for a cropped derivative the largest centered rectangle of the
 * derivative's aspect ratio.
 *
 * @param config_derivative Configured derivative. Must not be `NULL`.
 * @param width_px          Source width in pixels.
 * @param height_px         Source height in pixels.
 * @param x_out             Receives the region's left edge. Must not be `NULL`.
 * @param y_out             Receives the region's top edge. Must not be `NULL`.
 * @param width_out         Receives the region's width. Must not be `NULL`.
 * @param height_out        Receives the region's height. Must not be `NULL`.
 */
static void derivative_source_region(const struct GalleryDerivative* config_derivative,
                                     ptrdiff_t width_px,
                                     ptrdiff_t height_px,
                                     ptrdiff_t* x_out,
                                     ptrdiff_t* y_out,
                                     ptrdiff_t* width_out,
                                     ptrdiff_t* height_out) __attribute__((nonnull(1, 4, 5, 6, 7)));

/**
 * @brief Waits until `len` more bytes of decode memory fit `DECODE_BUDGET_LEN`, then reserves them.
 *
 * @param len Bytes to reserve, at most `DECODE_BUDGET_LEN`.
 */
static void reserve_decode_len(ptrdiff_t len);

/**
 * @brief Returns a reservation made by `reserve_decode_len` and wakes any waiting job.
 *
 * @param len Bytes to return, equal to the earlier reservation.
 */
static void release_decode_len(ptrdiff_t len);

/**
 * @brief Reports whether a derivative output exists, is non-empty, and is no older than its source.
 *
 * A missing or unreadable output counts as stale and needs no message: the regeneration that
 * follows reports its own failure with more context. The size cannot tell a finished derivative
 * from a cut one, so a derivative an interrupted build cut short is kept until its source changes.
 *
 * @param output_path Derivative output path to inspect.
 * @param source_stat What `os_stat` reported for the media source. Must not be `NULL`.
 * @param os          Host the output is on. Must not be `NULL`.
 * @return `true` when the existing output can be kept, otherwise `false`.
 */
static bool is_fresh_derivative(struct Str output_path,
                                const struct OsStat* source_stat,
                                struct Os* os) __attribute__((nonnull(2, 3)));

/**
 * @brief Reports whether an original copy exists, matches the source size, and is no older than it.
 *
 * The size comparison is the one check a derivative does not make. A copy is meant to be identical
 * to its source, so a differing size means a truncated or replaced copy even when the timestamp
 * looks current.
 *
 * @param output_path Copied-original output path to inspect.
 * @param source_stat What `os_stat` reported for the media source. Must not be `NULL`.
 * @param os          Host the copy is on. Must not be `NULL`.
 * @return `true` when the existing copy can be kept, otherwise `false`.
 */
static bool is_fresh_original(struct Str output_path,
                              const struct OsStat* source_stat,
                              struct Os* os) __attribute__((nonnull(2, 3)));

/**
 * @brief Compares two modification timestamps at nanosecond resolution.
 *
 * @param candidate What `os_stat` reported for the output being judged. Must not be `NULL`.
 * @param source    What `os_stat` reported for the source it must not predate. Must not be `NULL`.
 * @return `true` when `candidate` was modified at or after `source`, otherwise `false`.
 */
static bool is_at_least_as_new(const struct OsStat* candidate, const struct OsStat* source)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Resizes one region of an oriented image and writes it out as a WebP.
 *
 * The region is selected inside `oriented` rather than copied out first. The WebP is encoded into
 * `scratch` and then written whole in one `os_write`, so an encode failure writes nothing. The
 * resized image is released before returning, on success and on failure alike.
 *
 * @param oriented         Oriented source image, at its decoded scale. Must not be `NULL`.
 * @param source_x_px      Left edge of the source region, in oriented pixels.
 * @param source_y_px      Top edge of the source region, in oriented pixels.
 * @param source_width_px  Width of the source region. Must be positive and inside `oriented`.
 * @param source_height_px Height of the source region. Must be positive and inside `oriented`.
 * @param width_px         Output width in pixels, from 1 through `IMAGE_ENCODED_DIMENSION_MAX`.
 * @param height_px        Output height in pixels, from 1 through `IMAGE_ENCODED_DIMENSION_MAX`.
 * @param quality          WebP quality from 1 through 100.
 * @param output_path      Destination path, whose parent directories are created as needed.
 * @param os               Host the output is written to. Must not be `NULL`.
 * @param scratch          Arena the encoded WebP is held in until it is written.
 * @param operation        Operation leading the failure message, which names `output_path`.
 * @param log              Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a resize, encode, or write failure.
 */
static int write_derivative(const struct Image* oriented,
                            ptrdiff_t source_x_px,
                            ptrdiff_t source_y_px,
                            ptrdiff_t source_width_px,
                            ptrdiff_t source_height_px,
                            ptrdiff_t width_px,
                            ptrdiff_t height_px,
                            ptrdiff_t quality,
                            struct Str output_path,
                            struct Os* os,
                            struct Arena scratch,
                            struct Str operation,
                            struct Log* log) __attribute__((nonnull(1, 10, 13)));

int derivative_renderer_generate(const struct GalleryConfig* gallery_config,
                                 struct MediaItem* const* media,
                                 ptrdiff_t media_count,
                                 struct VideoFrame* video_frames,
                                 ptrdiff_t worker_count,
                                 struct JobArenas* worker_arenas,
                                 bool is_verbose,
                                 struct Os* os,
                                 struct Arena scratch,
                                 struct Log* log) {
  struct DerivativeContext context = {
      .gallery_config = gallery_config,
      .media = media,
      .video_frames = video_frames,
      .os = os,
  };
  return job_run(media_count, worker_count, worker_arenas, render_derivatives_job, &context,
                 S("generating derivatives"), (struct Str){0}, is_verbose, os, scratch, log);
}

ptrdiff_t derivative_renderer_decode_scale(const struct GalleryConfig* gallery_config,
                                           const struct MediaItem* item,
                                           const bool* is_stale,
                                           ptrdiff_t width_px,
                                           ptrdiff_t height_px,
                                           enum ImageOrientation orientation) {
  for (ptrdiff_t scale_eighths = 1; scale_eighths < IMAGE_SCALE_FULL; scale_eighths++) {
    ptrdiff_t width_stored = 0;
    ptrdiff_t height_stored = 0;
    image_scale_size(width_px, height_px, scale_eighths, &width_stored, &height_stored);
    ptrdiff_t width = 0;
    ptrdiff_t height = 0;
    image_orient_size(orientation, width_stored, height_stored, &width, &height);
    bool is_enough = true;
    for (ptrdiff_t i = 0; i < item->derivative_count && is_enough; i++) {
      if (!is_stale[i]) {
        continue;
      }
      ptrdiff_t region_x = 0;
      ptrdiff_t region_y = 0;
      ptrdiff_t region_width = 0;
      ptrdiff_t region_height = 0;
      derivative_source_region(&gallery_config->derivatives[i], width, height, &region_x, &region_y,
                               &region_width, &region_height);
      is_enough = region_width >= DECODE_SCALE_HEADROOM * item->derivatives[i].width_px &&
                  region_height >= DECODE_SCALE_HEADROOM * item->derivatives[i].height_px;
    }
    if (is_enough) {
      return scale_eighths;
    }
  }
  return IMAGE_SCALE_FULL;
}

static int render_derivatives_job(ptrdiff_t index,
                                  struct JobArenas* arenas,
                                  struct Log* log,
                                  void* userdata) {
  const struct DerivativeContext* context = userdata;
  memory_oom_set_job(context->media[index]->source_path, log);
  const int rc = render_derivatives_outputs(context, index, arenas->scratch, log);
  // Best effort is deliberate. A frame whose unlink fails stays marked, so the final cleanup tries
  // again, and a transient unlink failure cannot replace the job's useful first diagnostic.
  video_frame_remove(&context->video_frames[index]);
  return rc;
}

static int render_derivatives_outputs(const struct DerivativeContext* context,
                                      ptrdiff_t index,
                                      struct Arena scratch,
                                      struct Log* log) {
  const struct MediaItem* item = context->media[index];
  struct OsStat source_stat;
  if (render_derivatives_outputs_inspect(item->source_path, context->os, &source_stat, log) != 0) {
    return -1;
  }

  // `add_media` gives the item one `MediaDerivative` per configured derivative in the same order,
  // which is what lets the write loop read both arrays at the same index.
  bool is_stale[GALLERY_DERIVATIVE_COUNT_MAX] = {false};
  bool has_stale = false;
  for (ptrdiff_t i = 0; i < item->derivative_count; i++) {
    is_stale[i] = !is_fresh_derivative(item->derivatives[i].path, &source_stat, context->os);
    has_stale = has_stale || is_stale[i];
  }

  int rc = 0;
  if (has_stale && render_derivatives_images(context, index, is_stale, scratch, log) != 0) {
    rc = -1;
  }
  const struct OsSource original = {.kind = OS_SOURCE_FILE, .file_path = item->source_path};
  if (!is_fresh_original(item->original_path, &source_stat, context->os) &&
      os_write(context->os, item->original_path, original, S("failed to copy original"), log) !=
          0) {
    rc = -1;
  }
  return rc;
}

static int render_derivatives_outputs_inspect(struct Str source_path,
                                              struct Os* os,
                                              struct OsStat* stat_out,
                                              struct Log* log) {
  if (os_stat(os, source_path, stat_out) != 0) {
    char message[ERROR_SYSTEM_MESSAGE_SIZE];
    return log_error(log, "failed to inspect media source: %s ('%.*s')",
                     error_system_message(message, sizeof(message), errno), STR_FMT(source_path));
  }
  if (stat_out->is_dir) {
    return log_error(log, "failed to inspect media source: not a regular file ('%.*s')",
                     STR_FMT(source_path));
  }
  return 0;
}

static int render_derivatives_images(const struct DerivativeContext* context,
                                     ptrdiff_t index,
                                     const bool* is_stale,
                                     struct Arena scratch,
                                     struct Log* log) {
  const struct MediaItem* item = context->media[index];
  const struct VideoFrame* frame = &context->video_frames[index];
  if (item->kind == MEDIA_KIND_VIDEO && !frame->is_created) {
    return log_error(log, "video frame is unavailable ('%.*s')", STR_FMT(item->source_path));
  }
  const struct Str derivative_source_path =
      item->kind == MEDIA_KIND_VIDEO ? str_from_cstr(frame->path) : item->source_path;
  // The bytes stay in `scratch` until the job returns, unless `decode_oriented` gives a large
  // source's pages back. The decode allocates nothing in an arena.
  struct Str data = {0};
  if (os_read(context->os, derivative_source_path, IMAGE_INPUT_LEN_MAX, &scratch, &data,
              S("failed to read derivative source"), log) != 0) {
    return -1;
  }
  struct ImageHeader header;
  if (image_probe(data, &header, S("failed to decode derivative source"), derivative_source_path,
                  log) != 0) {
    return -1;
  }

  // An image source is oriented by its EXIF tag. A video frame is already upright, because it was
  // extracted auto-rotated.
  const enum ImageOrientation orientation =
      item->kind == MEDIA_KIND_IMAGE ? exif_read_orientation(data) : IMAGE_ORIENTATION_TOP_LEFT;
  const ptrdiff_t scale_eighths = derivative_renderer_decode_scale(
      context->gallery_config, item, is_stale, header.width_px, header.height_px, orientation);
  // The reservation uses the header of the bytes about to be decoded, not the dimensions the probe
  // phase recorded, so a source replaced since then cannot reserve less than it decodes. A
  // reservation above the budget, which only a derivative of more than about 55 million pixels
  // makes, would wait forever, so it is capped, and that job runs alone.
  ptrdiff_t width = 0;
  ptrdiff_t height = 0;
  image_scale_size(header.width_px, header.height_px, scale_eighths, &width, &height);
  ptrdiff_t derivative_pixel_count_max = 0;
  for (ptrdiff_t i = 0; i < item->derivative_count; i++) {
    const ptrdiff_t pixel_count = item->derivatives[i].width_px * item->derivatives[i].height_px;
    if (is_stale[i] && pixel_count > derivative_pixel_count_max) {
      derivative_pixel_count_max = pixel_count;
    }
  }
  const ptrdiff_t write_len =
      width * height * IMAGE_CHANNEL_COUNT + derivative_pixel_count_max * IMAGE_ENCODE_LEN_PER_PX;
  ptrdiff_t decode_len = image_decode_len(&header, scale_eighths, orientation);
  if (write_len > decode_len) {
    decode_len = write_len;
  }
  if (decode_len > DECODE_BUDGET_LEN) {
    decode_len = DECODE_BUDGET_LEN;
  }
  reserve_decode_len(decode_len);
  struct Image image = {0};
  int rc = decode_oriented(data, scale_eighths, orientation, &scratch, &image,
                           derivative_source_path, log);
  if (rc == 0) {
    rc = write_stale_derivatives(context, index, is_stale, &image, scratch, log);
  }
  image_free(&image);
  release_decode_len(decode_len);
  return rc;
}

static int decode_oriented(struct Str data,
                           ptrdiff_t scale_eighths,
                           enum ImageOrientation orientation,
                           const struct Arena* scratch,
                           struct Image* image_out,
                           struct Str derivative_source_path,
                           struct Log* log) {
  if (image_decode(data, scale_eighths, image_out, S("failed to decode derivative source"),
                   derivative_source_path, log) != 0) {
    return -1;
  }
  // The decode was the last read of the source. The worker's scratch outlives the job, so a large
  // source's pages would otherwise stay resident beside other workers' decodes and beside a rotated
  // image's second buffer.
  if (data.len >= JOB_SOURCE_DISCARD_LEN_MIN) {
    arena_discard(scratch, data.data, data.len);
  }
  image_orient(image_out, orientation);
  return 0;
}

static int write_stale_derivatives(const struct DerivativeContext* context,
                                   ptrdiff_t index,
                                   const bool* is_stale,
                                   const struct Image* oriented,
                                   struct Arena scratch,
                                   struct Log* log) {
  // Peak memory per job is the larger of the decode and orientation, as `image_decode_len` counts
  // them, after a source of `JOB_SOURCE_DISCARD_LEN_MIN` or more was given back, and the largest
  // derivative's resize and encode beside the oriented source, as `IMAGE_ENCODE_LEN_PER_PX` counts
  // them. An upright source is oriented in place. The resizer adds at most three padding bytes per
  // row for aligned stb stores. The encoder's own memory is released after each derivative, and the
  // encoded WebP is held in the job's scratch until its one write.
  const struct MediaItem* item = context->media[index];
  int rc = 0;
  for (ptrdiff_t i = 0; i < item->derivative_count; i++) {
    if (!is_stale[i]) {
      continue;
    }
    const struct MediaDerivative* derivative = &item->derivatives[i];
    const struct GalleryDerivative* config_derivative = &context->gallery_config->derivatives[i];
    ptrdiff_t crop_x = 0;
    ptrdiff_t crop_y = 0;
    ptrdiff_t crop_width = 0;
    ptrdiff_t crop_height = 0;
    derivative_source_region(config_derivative, oriented->width_px, oriented->height_px, &crop_x,
                             &crop_y, &crop_width, &crop_height);
    // Each derivative's operation lives in a copy of `scratch`, which the next one reuses.
    struct Arena operation_scratch = scratch;
    struct Buf operation = {.arena = &operation_scratch};
    buf_printf(&operation, "failed to write derivative '%.*s'", STR_FMT(derivative->name));
    if (write_derivative(oriented, crop_x, crop_y, crop_width, crop_height, derivative->width_px,
                         derivative->height_px, config_derivative->quality, derivative->path,
                         context->os, operation_scratch, buf_str(&operation), log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

static void derivative_source_region(const struct GalleryDerivative* config_derivative,
                                     ptrdiff_t width_px,
                                     ptrdiff_t height_px,
                                     ptrdiff_t* x_out,
                                     ptrdiff_t* y_out,
                                     ptrdiff_t* width_out,
                                     ptrdiff_t* height_out) {
  *x_out = 0;
  *y_out = 0;
  *width_out = width_px;
  *height_out = height_px;
  if (config_derivative->is_crop) {
    image_center_crop(width_px, height_px, config_derivative->width_px,
                      config_derivative->height_px, x_out, y_out, width_out, height_out);
  }
}

static void reserve_decode_len(ptrdiff_t len) {
  (void)pthread_mutex_lock(&decode_budget.mutex);
  while (decode_budget.len_reserved > DECODE_BUDGET_LEN - len) {
    (void)pthread_cond_wait(&decode_budget.released, &decode_budget.mutex);
  }
  decode_budget.len_reserved += len;
  (void)pthread_mutex_unlock(&decode_budget.mutex);
}

static void release_decode_len(ptrdiff_t len) {
  (void)pthread_mutex_lock(&decode_budget.mutex);
  decode_budget.len_reserved -= len;
  (void)pthread_cond_broadcast(&decode_budget.released);
  (void)pthread_mutex_unlock(&decode_budget.mutex);
}

static bool is_fresh_derivative(struct Str output_path,
                                const struct OsStat* source_stat,
                                struct Os* os) {
  // A missing output is the ordinary stale case.
  struct OsStat output_stat;
  return os_stat(os, output_path, &output_stat) == 0 && !output_stat.is_dir &&
         output_stat.len > 0 && is_at_least_as_new(&output_stat, source_stat);
}

static bool is_fresh_original(struct Str output_path,
                              const struct OsStat* source_stat,
                              struct Os* os) {
  struct OsStat output_stat;
  return os_stat(os, output_path, &output_stat) == 0 && !output_stat.is_dir &&
         output_stat.len > 0 && output_stat.len == source_stat->len &&
         is_at_least_as_new(&output_stat, source_stat);
}

static bool is_at_least_as_new(const struct OsStat* candidate, const struct OsStat* source) {
  return candidate->mtime_s > source->mtime_s ||
         (candidate->mtime_s == source->mtime_s && candidate->mtime_ns >= source->mtime_ns);
}

static int write_derivative(const struct Image* oriented,
                            ptrdiff_t source_x_px,
                            ptrdiff_t source_y_px,
                            ptrdiff_t source_width_px,
                            ptrdiff_t source_height_px,
                            ptrdiff_t width_px,
                            ptrdiff_t height_px,
                            ptrdiff_t quality,
                            struct Str output_path,
                            struct Os* os,
                            struct Arena scratch,
                            struct Str operation,
                            struct Log* log) {
  struct Image resized = {0};
  if (image_resize_region(oriented, source_x_px, source_y_px, source_width_px, source_height_px,
                          width_px, height_px, &resized, operation, output_path, log) != 0) {
    return -1;
  }
  struct Str webp = {0};
  int rc = image_encode_webp(&resized, quality, &scratch, &webp, operation, output_path, log);
  image_free(&resized);
  if (rc == 0) {
    rc = os_write(os, output_path, (struct OsSource){.bytes = webp}, operation, log);
  }
  return rc;
}
