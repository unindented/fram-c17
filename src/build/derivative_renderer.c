#include "build/derivative_renderer.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include "build/job.h"
#include "domain/gallery_config.h"
#include "domain/media_item.h"
#include "formats/exif.h"
#include "formats/image.h"
#include "runtime/fs.h"

/**
 * Largest total pixel count that derivative jobs hold decoded at once, across every worker.
 *
 * A job holds a decoded source from the start of its decode to the end of its last derivative
 * write, and its memory grows with the source's pixel count rather than its file size. A JPEG of a
 * few hundred bytes can declare `IMAGE_PIXEL_COUNT_MAX` pixels, and stb then zero-fills the scans
 * the file lacks. Measured in a release build at that size, one decode peaks at 573 MB for a
 * baseline JPEG and 1146 MB for a progressive one, whose coefficient buffers stb allocates in full.
 * `IMAGE_PIXEL_COUNT_MAX` bounds one decode but not how many run together, and one per worker
 * multiplies it by the core count: eight such files take a build on 10 cores to 9.9 GB peak RSS
 * with no shared bound.
 *
 * Two maximum-size decodes fit, so worst-case decode memory is about 2.3 GB whatever the worker
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
  size_t pixel_count_reserved;
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
   * Extracted video frame paths, one per job index. A job reads its own frame and unlinks it, while
   * the caller keeps ownership of the path strings.
   */
  char* const* video_frame_paths;

  /**
   * Decode hint width, the largest configured derivative width. It spans every configured
   * derivative, not only the stale ones, so the decoded source does not depend on which outputs
   * happen to be missing.
   */
  size_t decode_width_px;

  /** Decode hint height, the largest configured derivative height. */
  size_t decode_height_px;
};

/**
 * @brief Generates one media item's stale derivatives and copies its original when out of date.
 *
 * This is the `JobFn` of the derivative phase, so it runs concurrently with other indexes and
 * writes only the outputs belonging to `index`. A video's extracted frame is unlinked before
 * returning on every path, while the caller keeps the path string itself.
 *
 * @param jobs     Running job set, whose error slot for `index` the job may fill through
 *                 `job_set_error`. Must not be `NULL`.
 * @param index    Media index this job handles. Must be below the media count.
 * @param userdata `struct DerivativeContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording the failure through `job_set_error`.
 */
static int render_derivatives_job(struct JobSet* jobs, size_t index, void* userdata)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Writes one media item's stale derivatives and its stale original copy.
 *
 * An output already at least as new as the source is skipped, and the source is read, decoded, and
 * oriented at most once for the whole item. Each failure is latched through `job_set_error` and the
 * job carries on, so a failing derivative does not hide a failing original copy.
 *
 * @param context Shared derivative-phase state. Must not be `NULL`.
 * @param jobs    Running job set receiving failures. Must not be `NULL`.
 * @param index   Media index to render.
 * @return `0` when nothing failed, or `-1` after recording at least one failure.
 */
static int render_derivatives_outputs(const struct DerivativeContext* context,
                                      struct JobSet* jobs,
                                      size_t index) __attribute__((nonnull(1, 2)));

/**
 * @brief Reads, decodes, and orients one item's source, then writes each of its stale derivatives.
 *
 * The decode runs under a `decode_budget` reservation for the source's pixel count, held until the
 * decoded image is released.
 *
 * @param context  Shared derivative-phase state. Must not be `NULL`.
 * @param jobs     Running job set receiving failures. Must not be `NULL`.
 * @param index    Media index whose derivatives are written.
 * @param is_stale One flag per derivative of the item, `true` where the output must be rewritten.
 *                 Must not be `NULL`.
 * @return `0` when every stale derivative was written, or `-1` after recording a failure.
 */
static int render_derivatives_images(const struct DerivativeContext* context,
                                     struct JobSet* jobs,
                                     size_t index,
                                     const bool* is_stale) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Decodes a JPEG source at the build's decode hint and applies its orientation.
 *
 * An image source is oriented by its EXIF tag. A video frame is already upright, because it was
 * extracted auto-rotated.
 *
 * @param context           Shared derivative-phase state. Must not be `NULL`.
 * @param jobs              Running job set receiving failures. Must not be `NULL`.
 * @param index             Media index whose error slot receives failures.
 * @param derivative_source Path the JPEG bytes were read from, for diagnostics. Must not be `NULL`.
 * @param data              JPEG bytes of the source image or video frame. Must not be `NULL`.
 * @param data_len          Number of bytes at `data`.
 * @param image_out         Receives the oriented image, which the caller releases with `image_free`
 *                          on success and failure alike. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a decode or orientation failure.
 */
static int decode_oriented(const struct DerivativeContext* context,
                           struct JobSet* jobs,
                           size_t index,
                           const char* derivative_source,
                           const unsigned char* data,
                           size_t data_len,
                           struct Image* image_out) __attribute__((nonnull(1, 2, 4, 5, 7)));

/**
 * @brief Writes every stale derivative of one item from its oriented source.
 *
 * Each failed derivative is recorded and the loop carries on, so one failure does not hide another.
 *
 * @param context  Shared derivative-phase state. Must not be `NULL`.
 * @param jobs     Running job set receiving failures. Must not be `NULL`.
 * @param index    Media index whose derivatives are written.
 * @param is_stale One flag per derivative of the item, `true` where the output must be rewritten.
 *                 Must not be `NULL`.
 * @param oriented Oriented full-size source image. Must not be `NULL`.
 * @return `0` when every stale derivative was written, or `-1` after recording a failure.
 */
static int write_stale_derivatives(const struct DerivativeContext* context,
                                   struct JobSet* jobs,
                                   size_t index,
                                   const bool* is_stale,
                                   const struct Image* oriented)
    __attribute__((nonnull(1, 2, 4, 5)));

/**
 * @brief Waits until `pixel_count` more decoded pixels fit `DECODE_PIXEL_BUDGET`, then reserves
 *        them.
 *
 * @param pixel_count Pixels to reserve, at most `IMAGE_PIXEL_COUNT_MAX`.
 */
static void reserve_decode_pixels(size_t pixel_count);

/**
 * @brief Returns a reservation made by `reserve_decode_pixels` and wakes any waiting job.
 *
 * @param pixel_count Pixels to return, equal to the earlier reservation.
 */
static void release_decode_pixels(size_t pixel_count);

/**
 * @brief Reports whether a derivative output exists, is non-empty, and is no older than its source.
 *
 * A missing or unreadable output counts as stale, so no reason is collected: the regeneration that
 * follows reports its own failure with more context.
 *
 * @param output_path Derivative output path to inspect. Must not be `NULL`.
 * @param source_meta Metadata of the media source it is derived from. Must not be `NULL`.
 * @return `true` when the existing output can be kept, otherwise `false`.
 */
static bool is_fresh_derivative(const char* output_path, const struct FsMeta* source_meta)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether an original copy exists, matches the source size, and is no older than it.
 *
 * The size comparison is the one check a derivative does not make. A copy is meant to be identical
 * to its source, so a differing size means a truncated or replaced copy even when the timestamp
 * looks current.
 *
 * @param output_path Copied-original output path to inspect. Must not be `NULL`.
 * @param source_meta Metadata of the media source it was copied from. Must not be `NULL`.
 * @return `true` when the existing copy can be kept, otherwise `false`.
 */
static bool is_fresh_original(const char* output_path, const struct FsMeta* source_meta)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Compares two modification timestamps at nanosecond resolution.
 *
 * @param candidate Metadata of the output being judged. Must not be `NULL`.
 * @param source    Metadata of the source it must not predate. Must not be `NULL`.
 * @return `true` when `candidate` was modified at or after `source`, otherwise `false`.
 */
static bool is_at_least_as_new(const struct FsMeta* candidate, const struct FsMeta* source)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Resizes one region of an oriented image and writes it out as a JPEG.
 *
 * The region is selected inside `oriented` rather than copied out first. The encoded bytes and the
 * resized image are both released before returning, on success and on failure alike.
 *
 * @param oriented         Oriented full-size source image. Must not be `NULL`.
 * @param source_x_px      Left edge of the source region, in oriented pixels.
 * @param source_y_px      Top edge of the source region, in oriented pixels.
 * @param source_width_px  Width of the source region. Must be positive and inside `oriented`.
 * @param source_height_px Height of the source region. Must be positive and inside `oriented`.
 * @param width_px         Output width in pixels, from 1 through `IMAGE_DIMENSION_MAX`.
 * @param height_px        Output height in pixels, from 1 through `IMAGE_DIMENSION_MAX`.
 * @param quality          JPEG quality from 1 through 100.
 * @param output_path      Destination path, whose parent directories are created as needed. Must
 *                         not be `NULL`.
 * @param reason           Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len       Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a resize, encode, or write failure.
 */
static int write_derivative(const struct Image* oriented,
                            size_t source_x_px,
                            size_t source_y_px,
                            size_t source_width_px,
                            size_t source_height_px,
                            size_t width_px,
                            size_t height_px,
                            size_t quality,
                            const char* output_path,
                            char* reason,
                            size_t reason_len) __attribute__((nonnull(1, 9)));

int derivative_renderer_generate(const struct GalleryConfig* gallery_config,
                                 struct MediaItem* const* media,
                                 size_t media_count,
                                 char* const* video_frame_paths,
                                 size_t worker_count,
                                 bool is_verbose,
                                 struct StringBuffer* error_out) {
  struct DerivativeContext context = {
      .gallery_config = gallery_config,
      .media = media,
      .video_frame_paths = video_frame_paths,
  };
  for (size_t i = 0; i < gallery_config->derivative_count; i++) {
    const struct GalleryDerivative* derivative = &gallery_config->derivatives[i];
    if (derivative->width_px > context.decode_width_px) {
      context.decode_width_px = derivative->width_px;
    }
    if (derivative->height_px > context.decode_height_px) {
      context.decode_height_px = derivative->height_px;
    }
  }
  return job_run(media_count, worker_count, render_derivatives_job, &context,
                 "generating derivatives", is_verbose, error_out);
}

static int render_derivatives_job(struct JobSet* jobs, size_t index, void* userdata) {
  const struct DerivativeContext* context = userdata;
  const int rc = render_derivatives_outputs(context, jobs, index);
  const char* video_frame_path = context->video_frame_paths[index];
  if (context->media[index]->kind == MEDIA_KIND_VIDEO && video_frame_path != NULL) {
    // Best effort is deliberate. The caller still owns the path and its final cleanup unlinks it
    // again, so a transient unlink failure cannot replace the job's useful first diagnostic.
    (void)unlink(video_frame_path);
  }
  return rc;
}

static int render_derivatives_outputs(const struct DerivativeContext* context,
                                      struct JobSet* jobs,
                                      size_t index) {
  const struct MediaItem* item = context->media[index];
  struct FsMeta source_meta;
  char reason[FS_REASON_SIZE];
  if (fs_stat_meta(item->source_path, &source_meta, reason, sizeof(reason)) != 0) {
    job_set_error(jobs, index, "failed to inspect media source: %s ('%s')", reason,
                  item->source_path);
    return -1;
  }

  // `add_media` gives the item one `MediaDerivative` per configured derivative in the same order,
  // which is what lets the write loop read both arrays at the same index.
  bool is_stale[GALLERY_DERIVATIVE_COUNT_MAX] = {false};
  bool has_stale = false;
  for (size_t i = 0; i < item->derivative_count; i++) {
    is_stale[i] = !is_fresh_derivative(item->derivatives[i].path, &source_meta);
    has_stale = has_stale || is_stale[i];
  }

  int rc = 0;
  if (has_stale && render_derivatives_images(context, jobs, index, is_stale) != 0) {
    rc = -1;
  }
  if (!is_fresh_original(item->original_path, &source_meta) &&
      fs_copy_file(item->source_path, item->original_path, reason, sizeof(reason)) != 0) {
    job_set_error(jobs, index, "failed to copy original: %s ('%s')", reason, item->original_path);
    rc = -1;
  }
  return rc;
}

static int render_derivatives_images(const struct DerivativeContext* context,
                                     struct JobSet* jobs,
                                     size_t index,
                                     const bool* is_stale) {
  const struct MediaItem* item = context->media[index];
  const char* derivative_source =
      item->kind == MEDIA_KIND_VIDEO ? context->video_frame_paths[index] : item->source_path;
  if (derivative_source == NULL) {
    job_set_error(jobs, index, "video frame is unavailable ('%s')", item->source_path);
    return -1;
  }
  unsigned char* data = NULL;
  size_t data_len = 0;
  char reason[FS_REASON_SIZE];
  if (fs_read_file(derivative_source, IMAGE_INPUT_LEN_MAX, &data, &data_len, reason,
                   sizeof(reason)) != 0) {
    job_set_error(jobs, index, "failed to read derivative source: %s ('%s')", reason,
                  derivative_source);
    return -1;
  }
  size_t source_width = 0;
  size_t source_height = 0;
  if (image_probe(data, data_len, &source_width, &source_height, reason, sizeof(reason)) != 0) {
    job_set_error(jobs, index, "failed to decode derivative source: %s ('%s')", reason,
                  derivative_source);
    free(data);
    return -1;
  }

  // The reservation uses the dimensions of the bytes about to be decoded, not the ones the probe
  // phase recorded, so a source replaced since then cannot reserve less than it decodes.
  // `image_decode` rejects bytes whose decoded dimensions differ from these.
  const size_t pixel_count = source_width * source_height;
  reserve_decode_pixels(pixel_count);
  struct Image image = {0};
  int rc = decode_oriented(context, jobs, index, derivative_source, data, data_len, &image);
  free(data);
  if (rc == 0) {
    rc = write_stale_derivatives(context, jobs, index, is_stale, &image);
  }
  image_free(&image);
  release_decode_pixels(pixel_count);
  return rc;
}

static int decode_oriented(const struct DerivativeContext* context,
                           struct JobSet* jobs,
                           size_t index,
                           const char* derivative_source,
                           const unsigned char* data,
                           size_t data_len,
                           struct Image* image_out) {
  char reason[FS_REASON_SIZE];
  if (image_decode(data, data_len, context->decode_width_px, context->decode_height_px, image_out,
                   reason, sizeof(reason)) != 0) {
    job_set_error(jobs, index, "failed to decode derivative source: %s ('%s')", reason,
                  derivative_source);
    return -1;
  }
  const enum ImageOrientation orientation = context->media[index]->kind == MEDIA_KIND_IMAGE
                                                ? exif_read_orientation(data, data_len)
                                                : IMAGE_ORIENTATION_TOP_LEFT;
  if (image_orient(image_out, orientation, reason, sizeof(reason)) != 0) {
    job_set_error(jobs, index, "failed to orient derivative source: %s ('%s')", reason,
                  derivative_source);
    return -1;
  }
  return 0;
}

static int write_stale_derivatives(const struct DerivativeContext* context,
                                   struct JobSet* jobs,
                                   size_t index,
                                   const bool* is_stale,
                                   const struct Image* oriented) {
  // Peak memory per job is the decode itself, described at `DECODE_PIXEL_BUDGET`. After it the job
  // holds the oriented source, one resized image, and the growing encoded JPEG. An upright source
  // is oriented in place, and a rotated or mirrored one briefly holds two full-size buffers while
  // `image_orient` transforms it. The resizer adds at most three padding bytes per row for aligned
  // stb stores, and `IMAGE_ENCODED_LEN_MAX` bounds the encoder buffer.
  const struct MediaItem* item = context->media[index];
  int rc = 0;
  for (size_t i = 0; i < item->derivative_count; i++) {
    if (!is_stale[i]) {
      continue;
    }
    const struct MediaDerivative* derivative = &item->derivatives[i];
    const struct GalleryDerivative* config_derivative = &context->gallery_config->derivatives[i];
    size_t crop_x = 0;
    size_t crop_y = 0;
    size_t crop_width = oriented->width_px;
    size_t crop_height = oriented->height_px;
    if (config_derivative->is_crop) {
      image_center_crop(oriented->width_px, oriented->height_px, config_derivative->width_px,
                        config_derivative->height_px, &crop_x, &crop_y, &crop_width, &crop_height);
    }
    char reason[FS_REASON_SIZE];
    if (write_derivative(oriented, crop_x, crop_y, crop_width, crop_height, derivative->width_px,
                         derivative->height_px, config_derivative->quality, derivative->path,
                         reason, sizeof(reason)) != 0) {
      job_set_error(jobs, index, "failed to write derivative '%s': %s ('%s')", derivative->name,
                    reason, derivative->path);
      rc = -1;
    }
  }
  return rc;
}

static void reserve_decode_pixels(size_t pixel_count) {
  (void)pthread_mutex_lock(&decode_budget.mutex);
  while (decode_budget.pixel_count_reserved > DECODE_PIXEL_BUDGET - pixel_count) {
    (void)pthread_cond_wait(&decode_budget.released, &decode_budget.mutex);
  }
  decode_budget.pixel_count_reserved += pixel_count;
  (void)pthread_mutex_unlock(&decode_budget.mutex);
}

static void release_decode_pixels(size_t pixel_count) {
  (void)pthread_mutex_lock(&decode_budget.mutex);
  decode_budget.pixel_count_reserved -= pixel_count;
  (void)pthread_cond_broadcast(&decode_budget.released);
  (void)pthread_mutex_unlock(&decode_budget.mutex);
}

static bool is_fresh_derivative(const char* output_path, const struct FsMeta* source_meta) {
  struct FsMeta output_meta;
  return fs_stat_meta(output_path, &output_meta, NULL, 0) == 0 && output_meta.size_len > 0 &&
         is_at_least_as_new(&output_meta, source_meta);
}

static bool is_fresh_original(const char* output_path, const struct FsMeta* source_meta) {
  struct FsMeta output_meta;
  return fs_stat_meta(output_path, &output_meta, NULL, 0) == 0 && output_meta.size_len > 0 &&
         output_meta.size_len == source_meta->size_len &&
         is_at_least_as_new(&output_meta, source_meta);
}

static bool is_at_least_as_new(const struct FsMeta* candidate, const struct FsMeta* source) {
  return candidate->mtime_s > source->mtime_s ||
         (candidate->mtime_s == source->mtime_s && candidate->mtime_ns >= source->mtime_ns);
}

static int write_derivative(const struct Image* oriented,
                            size_t source_x_px,
                            size_t source_y_px,
                            size_t source_width_px,
                            size_t source_height_px,
                            size_t width_px,
                            size_t height_px,
                            size_t quality,
                            const char* output_path,
                            char* reason,
                            size_t reason_len) {
  struct Image resized = {0};
  if (image_resize_region(oriented, source_x_px, source_y_px, source_width_px, source_height_px,
                          width_px, height_px, &resized, reason, reason_len) != 0) {
    return -1;
  }
  unsigned char* encoded = NULL;
  size_t encoded_len = 0;
  int rc = image_encode_jpeg(&resized, quality, &encoded, &encoded_len, reason, reason_len);
  if (rc == 0) {
    rc = fs_write_file(output_path, encoded, encoded_len, reason, reason_len);
  }
  free(encoded);
  image_free(&resized);
  return rc;
}
