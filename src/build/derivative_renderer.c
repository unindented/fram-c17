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
 * Largest total pixel count that derivative jobs hold decoded at once, across every worker.
 *
 * A job holds a decoded source from the start of its decode to the end of its last derivative
 * write, and its memory grows with the source's pixel count rather than its file size. A JPEG of a
 * few hundred bytes can declare `IMAGE_PIXEL_COUNT_MAX` pixels, and stb then zero-fills the scans
 * the file lacks. Measured in a release build at that size, one decode peaks at 573 MiB for a
 * baseline JPEG and 1146 MiB for a progressive one, whose coefficient buffers stb allocates in
 * full. `IMAGE_PIXEL_COUNT_MAX` bounds one decode but not how many run together, and one per worker
 * multiplies it by the core count: eight such files take a build on 10 cores to 9.9 GB peak RSS
 * with no shared bound.
 *
 * Two maximum-size decodes fit, so worst-case decode memory is about 2.2 GiB whatever the worker
 * count, and the same eight files peak at 2.4 GB. The bound still admits eight 24-megapixel photos
 * at once, so an ordinary gallery keeps one decode per core on most machines.
 */
enum { DECODE_PIXEL_BUDGET = 2 * IMAGE_PIXEL_COUNT_MAX };

// A job reserves its whole pixel count before decoding, so one image larger than the budget would
// wait forever.
_Static_assert((int)DECODE_PIXEL_BUDGET >= (int)IMAGE_PIXEL_COUNT_MAX,
               "a maximum-size image must fit the decode budget alone");

/** Pixel reservations shared by every derivative worker in the process. */
struct DecodeBudget {
  /** Guards `pixel_count_reserved`. */
  pthread_mutex_t mutex;

  /** Signalled whenever a job returns its reservation. */
  pthread_cond_t released;

  /** Pixels currently reserved by jobs holding a decoded source, at most `DECODE_PIXEL_BUDGET`. */
  ptrdiff_t pixel_count_reserved;
};

static struct DecodeBudget decode_budget = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .released = PTHREAD_COND_INITIALIZER,
    .pixel_count_reserved = 0,
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
 * worker's scratch, except the decoded pixels, which stay on `malloc` under the decode budget.
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
 * The decode runs under a `decode_budget` reservation for the source's pixel count, held until the
 * decoded image is released.
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
 * @brief Decodes a JPEG source and applies its orientation.
 *
 * An image source is oriented by its EXIF tag. A video frame is already upright, because it was
 * extracted auto-rotated. Once the orientation is read, the pixels hold all the derivatives need,
 * so a source of `JOB_SOURCE_DISCARD_LEN_MIN` or more gives its pages back before `image_orient`,
 * which may hold two full-size buffers.
 *
 * @param context                Shared derivative-phase state. Must not be `NULL`.
 * @param index                  Media index whose source is decoded.
 * @param derivative_source_path Path the JPEG bytes were read from, for diagnostics.
 * @param data                   JPEG bytes of the source image or video frame.
 * @param scratch                Arena `data` was read into. Must not be `NULL`.
 * @param image_out              Receives the oriented image, which the caller releases with
 *                               `image_free` on success and failure alike. Must not be `NULL`.
 * @param log                    Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a decode failure.
 */
static int decode_oriented(const struct DerivativeContext* context,
                           ptrdiff_t index,
                           struct Str derivative_source_path,
                           struct Str data,
                           const struct Arena* scratch,
                           struct Image* image_out,
                           struct Log* log) __attribute__((nonnull(1, 5, 6, 7)));

/**
 * @brief Writes every stale derivative of one item from its oriented source.
 *
 * Each failed derivative is recorded and the loop carries on, so one failure does not hide another.
 *
 * @param context  Shared derivative-phase state. Must not be `NULL`.
 * @param index    Media index whose derivatives are written.
 * @param is_stale One flag per derivative of the item, `true` where the output must be rewritten.
 *                 Must not be `NULL`.
 * @param oriented Oriented full-size source image. Must not be `NULL`.
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
 * @brief Waits until `pixel_count` more decoded pixels fit `DECODE_PIXEL_BUDGET`, then reserves
 *        them.
 *
 * @param pixel_count Pixels to reserve, at most `IMAGE_PIXEL_COUNT_MAX`.
 */
static void reserve_decode_pixels(ptrdiff_t pixel_count);

/**
 * @brief Returns a reservation made by `reserve_decode_pixels` and wakes any waiting job.
 *
 * @param pixel_count Pixels to return, equal to the earlier reservation.
 */
static void release_decode_pixels(ptrdiff_t pixel_count);

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
 * @brief Resizes one region of an oriented image and writes it out as a JPEG.
 *
 * The region is selected inside `oriented` rather than copied out first. The JPEG is encoded into
 * `scratch` and then written whole in one `os_write`, so an encode failure writes nothing. The
 * resized image is released before returning, on success and on failure alike.
 *
 * @param oriented         Oriented full-size source image. Must not be `NULL`.
 * @param source_x_px      Left edge of the source region, in oriented pixels.
 * @param source_y_px      Top edge of the source region, in oriented pixels.
 * @param source_width_px  Width of the source region. Must be positive and inside `oriented`.
 * @param source_height_px Height of the source region. Must be positive and inside `oriented`.
 * @param width_px         Output width in pixels, from 1 through `IMAGE_DIMENSION_MAX`.
 * @param height_px        Output height in pixels, from 1 through `IMAGE_DIMENSION_MAX`.
 * @param quality          JPEG quality from 1 through 100.
 * @param output_path      Destination path, whose parent directories are created as needed.
 * @param os               Host the output is written to. Must not be `NULL`.
 * @param scratch          Arena the encoded JPEG is held in until it is written.
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
  ptrdiff_t source_width = 0;
  ptrdiff_t source_height = 0;
  if (image_probe(data, &source_width, &source_height, S("failed to decode derivative source"),
                  derivative_source_path, log) != 0) {
    return -1;
  }

  // The reservation uses the dimensions of the bytes about to be decoded, not the ones the probe
  // phase recorded, so a source replaced since then cannot reserve less than it decodes.
  // `image_decode` rejects bytes whose decoded dimensions differ from these.
  const ptrdiff_t pixel_count = source_width * source_height;
  reserve_decode_pixels(pixel_count);
  struct Image image = {0};
  int rc = decode_oriented(context, index, derivative_source_path, data, &scratch, &image, log);
  if (rc == 0) {
    rc = write_stale_derivatives(context, index, is_stale, &image, scratch, log);
  }
  image_free(&image);
  release_decode_pixels(pixel_count);
  return rc;
}

static int decode_oriented(const struct DerivativeContext* context,
                           ptrdiff_t index,
                           struct Str derivative_source_path,
                           struct Str data,
                           const struct Arena* scratch,
                           struct Image* image_out,
                           struct Log* log) {
  if (image_decode(data, image_out, S("failed to decode derivative source"), derivative_source_path,
                   log) != 0) {
    return -1;
  }
  const enum ImageOrientation orientation = context->media[index]->kind == MEDIA_KIND_IMAGE
                                                ? exif_read_orientation(data)
                                                : IMAGE_ORIENTATION_TOP_LEFT;
  // The orientation was the last read of the source. The worker's scratch outlives the job, so a
  // large source's pages would otherwise stay resident beside other workers' decodes and beside a
  // rotated image's second buffer.
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
  // Peak memory per job is the decode, described at `DECODE_PIXEL_BUDGET`, or for a rotated or
  // mirrored source the two full-size buffers `image_orient` briefly holds, after a source of
  // `JOB_SOURCE_DISCARD_LEN_MIN` or more was given back. After it the job holds the oriented source
  // and one resized image. An upright source is oriented in place. The resizer adds at most three
  // padding bytes per row for aligned stb stores. The encoded JPEG is held in the job's scratch
  // until its one write.
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
    ptrdiff_t crop_width = oriented->width_px;
    ptrdiff_t crop_height = oriented->height_px;
    if (config_derivative->is_crop) {
      image_center_crop(oriented->width_px, oriented->height_px, config_derivative->width_px,
                        config_derivative->height_px, &crop_x, &crop_y, &crop_width, &crop_height);
    }
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

static void reserve_decode_pixels(ptrdiff_t pixel_count) {
  (void)pthread_mutex_lock(&decode_budget.mutex);
  while (decode_budget.pixel_count_reserved > DECODE_PIXEL_BUDGET - pixel_count) {
    (void)pthread_cond_wait(&decode_budget.released, &decode_budget.mutex);
  }
  decode_budget.pixel_count_reserved += pixel_count;
  (void)pthread_mutex_unlock(&decode_budget.mutex);
}

static void release_decode_pixels(ptrdiff_t pixel_count) {
  (void)pthread_mutex_lock(&decode_budget.mutex);
  decode_budget.pixel_count_reserved -= pixel_count;
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
  struct Str jpeg = {0};
  int rc = image_encode_jpeg(&resized, quality, &scratch, &jpeg, operation, output_path, log);
  image_free(&resized);
  if (rc == 0) {
    rc = os_write(os, output_path, (struct OsSource){.bytes = jpeg}, operation, log);
  }
  return rc;
}
