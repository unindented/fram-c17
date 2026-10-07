#include "app/cmd_build.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build/album_scanner.h"
#include "build/derivative_renderer.h"
#include "build/manifest_builder.h"
#include "build/page_renderer.h"
#include "build/site_writer.h"
#include "core/error.h"
#include "core/path_list.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/manifest.h"
#include "domain/media_item.h"
#include "runtime/fs.h"
#include "runtime/pool.h"
#include "runtime/proc.h"
#include "shared/string_buffer.h"

/** One configured directory the build reads, which must be a usable directory when present. */
struct ReadRoot {
  /** Configured path, already slash-trimmed by the config load. */
  const char* path;

  /** Config key naming `path`, used by every diagnostic so the user knows what to edit. */
  const char* key;

  /** Whether a gallery may legitimately not have this directory. */
  bool is_optional;
};

/** Mutable state owned by one `cmd_build_execute` invocation. */
struct BuildState {
  /** Loaded gallery configuration, owning every configured path, template name, and derivative. */
  struct GalleryConfig gallery_config;

  /** Discovered media source paths below `input_dir`, sorted by the walk that produced them. */
  struct PathList source_paths;

  /** Discovered static file paths below `static_dir`, left empty when that directory is absent. */
  struct PathList static_paths;

  /** Intended output paths for this build, populated before any file is written. */
  struct Manifest manifest;

  /**
   * Scanned albums sorted by `source_dir`, with the root album at index `0`. Each album owns the
   * arena backing its own strings and media records, so `album_scanner_free_skeleton` releases the
   * array and every album in it.
   */
  struct Album** albums;

  /** Number of albums in `albums`, and therefore the number of `rendered_pages` slots. */
  size_t album_count;

  /**
   * Every media item of every album, in source order. The items live in their owning album's arena,
   * so only the array itself is freed, together with `albums`.
   */
  struct MediaItem** media;

  /** Number of media items in `media`, and therefore the number of `video_frame_paths` slots. */
  size_t media_count;

  /**
   * One slot per media item, holding the path of the still frame the probe extracted from a video.
   * A slot stays `NULL` for an image, and for a video that failed before its frame was extracted.
   * `album_scanner_free_video_frames` unlinks and frees the slots, and the array is freed after it.
   */
  char** video_frame_paths;

  /**
   * One slot per album, holding that album's rendered page. A slot's `html` stays `NULL` until the
   * page render fills it, and each is freed individually.
   */
  struct RenderedPage* rendered_pages;

  /** Worker threads for each parallel phase, already resolved to the detected count when unset. */
  size_t worker_count;

  /** Whether phase progress and status messages are printed to `stderr`. */
  bool is_verbose;
};

/**
 * @brief Prints a formatted status line to `stderr` when the build is verbose.
 *
 * @param state Build state whose verbosity gates the output. Must not be `NULL`.
 * @param fmt   `printf`-style format string. Must not be `NULL`.
 * @param ...   Arguments for `fmt`.
 */
static void build_verbose(const struct BuildState* state, const char* fmt, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Initializes every owned field in a build state so cleanup is safe after partial setup.
 *
 * @param state Build state to prepare. Must not be `NULL`.
 */
static void build_state_init(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Releases every allocation a build state owns and leaves the state zeroed.
 *
 * The rendered page array is bounded by `album_count`, the count it was allocated with, so a build
 * that failed before a page was rendered frees the same slots as one that wrote them all. The flat
 * media array holds pointers into the albums' arenas, so `album_scanner_free_skeleton` releases
 * both together.
 *
 * @param state Build state to release. Must not be `NULL`.
 */
static void build_state_free(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Loads `fram.toml`, rejects an `output_dir` inside an input tree, and discovers media and
 *        static files.
 *
 * The `output_dir` check runs before either walk, and both walks leave `output_dir` out, so a file
 * an earlier build generated is never read back as a source.
 *
 * @param state   Build state that receives the configuration, media source paths, and static file
 *                paths. Must not be `NULL`.
 * @param err     Destination buffer for a failure diagnostic.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` on a configuration, directory, output directory, listing, or
 *         missing-tool failure.
 */
static int load_build_inputs(struct BuildState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Requires every configured read root to be a usable directory.
 *
 * Walks the `ReadRoot` spec table, so each diagnostic names the config key the user has to edit. An
 * optional root that is not there is skipped, and any other root that cannot be resolved is
 * reported with its cause.
 *
 * @param state   Build state supplying the configured read roots. Must not be `NULL`.
 * @param err     Destination buffer for a failure diagnostic.
 * @param err_len Size of `err` in bytes.
 * @return `0` when every present root is a usable directory, or `-1` when one cannot be resolved or
 *         is not a directory.
 */
static int require_read_roots(const struct BuildState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Requires `ffmpeg` and `ffprobe` to be runnable when any source is a video.
 *
 * A gallery of images alone needs neither tool, so the probe runs only once a video source has been
 * discovered. Running both here reports a missing tool once, before the parallel probe and
 * derivative phases would report it per video.
 *
 * @param source_paths Discovered media source paths, scanned for a video source. Must not be
 *                     `NULL`.
 * @param err          Destination buffer for a failure diagnostic.
 * @param err_len      Size of `err` in bytes.
 * @return `0` when no source is a video or both tools ran, or `-1` when either could not be run.
 */
static int require_video_tools(const struct PathList* source_paths, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Builds the album and media skeleton and allocates one result slot per media item and per
 *        album.
 *
 * @param state   Build state that receives the albums, the flat media array, the video frame slots,
 *                and the rendered page slots. Must not be `NULL`.
 * @param err     Destination buffer for a failure diagnostic.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` on a scan, path-limit, or allocation failure.
 */
static int build_album_skeleton(struct BuildState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Probes every media item's metadata, dispatching jobs across worker threads.
 *
 * A gallery with no media skips the phase, so it prints no status line and starts no pool.
 *
 * @param state     Build state holding the media items and video frame slots. Must not be `NULL`.
 * @param error_out Growable buffer that receives the collected probe diagnostics. Must not be
 *                  `NULL`.
 * @return `0` when every job succeeded or there is no media, or `-1` when any probe job failed.
 */
static int probe_media(struct BuildState* state, struct StringBuffer* error_out)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Records every intended output path, rejecting a collision before anything is written.
 *
 * @param state   Build state whose manifest is populated from the configuration, the discovered
 *                inputs, and the scanned albums. Must not be `NULL`.
 * @param err     Destination buffer for a failure diagnostic.
 * @param err_len Size of `err` in bytes.
 * @return `0` when every output path is unique and names no build input, or `-1` on a collision, an
 *         input overwrite, or an allocation failure.
 */
static int populate_output_manifest(struct BuildState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Creates the output directory once the manifest has accepted every output path.
 *
 * Nothing before this phase writes, so a build the manifest or an earlier phase refuses leaves the
 * filesystem as it found it, without even an empty `output_dir`.
 *
 * @param state   Build state supplying `output_dir`. Must not be `NULL`.
 * @param err     Destination buffer for a failure diagnostic.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` when a component of `output_dir` cannot be created.
 */
static int prepare_output_dir(const struct BuildState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Generates every media item's derivatives, dispatching jobs across worker threads.
 *
 * A gallery with no media skips the phase, so it prints no status line and starts no pool.
 *
 * @param state     Build state holding the probed media items and video frame slots. Must not be
 *                  `NULL`.
 * @param error_out Growable buffer that receives the collected derivative diagnostics. Must not be
 *                  `NULL`.
 * @return `0` when every job succeeded or there is no media, or `-1` when any derivative job
 *         failed.
 */
static int generate_derivatives(struct BuildState* state, struct StringBuffer* error_out)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Renders every album's page, dispatching jobs across worker threads.
 *
 * A gallery always has its root album, so this phase never runs empty.
 *
 * @param state     Build state holding the scanned albums and rendered page slots. Must not be
 *                  `NULL`.
 * @param error_out Growable buffer that receives the collected render diagnostics. Must not be
 *                  `NULL`.
 * @return `0` when every job succeeded, or `-1` when any render job failed.
 */
static int render_album_pages(struct BuildState* state, struct StringBuffer* error_out)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the album pages, the aggregate template outputs, and the copied static files.
 *
 * @param state   Build state holding the rendered pages, the scanned albums, and the static file
 * paths. Must not be `NULL`.
 * @param err     Destination buffer for a failure diagnostic.
 * @param err_len Size of `err` in bytes.
 * @return `0` when every output was written, or `-1` on the first render or write failure.
 */
static int write_generated_site(struct BuildState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

enum ExitCode cmd_build_run(const struct BuildOptions* options) {
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  enum ExitCode rc = EXIT_CODE_FAILURE;
  if (cmd_build_execute(options, &error_buffer) != 0) {
    // `error_buffer` comes back empty only when recording the diagnostic itself ran out of memory,
    // so the fallback keeps the failure visible. The fallback also makes the discarded
    // `(void)string_buffer_append(...)` results in this file safe. An append that fails degrades
    // the message to this line instead of reporting a silent success.
    fprintf(stderr, "%s\n", error_buffer.data != NULL ? error_buffer.data : "build failed");
    goto cleanup;
  }
  rc = EXIT_CODE_OK;

cleanup:
  string_buffer_free(&error_buffer);
  return rc;
}

int cmd_build_execute(const struct BuildOptions* options, struct StringBuffer* error_out) {
  struct BuildState state;
  build_state_init(&state);
  state.worker_count =
      options->worker_count != 0 ? options->worker_count : pool_resolve_worker_count();
  state.is_verbose = options->is_verbose;

  // Single-message phases report through the fixed `err`/`err_len` convention and are bridged into
  // the growable buffer here. The parallel phases aggregate many failures and append directly.
  //
  // The order below follows the data: the probe records each item's derivative dimensions, which
  // the derivative and page phases read, and extracts each video's still frame, which the
  // derivative phase reads. Every source has to be probed before any derivative is generated or any
  // page is rendered.
  char err[ERROR_MESSAGE_SIZE];
  // Terminated before the first phase runs, so a phase that returns `-1` without formatting a
  // diagnostic cannot hand `string_buffer_append` an indeterminate `char[512]`. Reading one is
  // undefined behavior, and in practice appends stack garbage to the build's error output.
  err[0] = '\0';
  int rc = -1;
  if (load_build_inputs(&state, err, sizeof(err)) != 0) {
    (void)string_buffer_append(error_out, err);
    goto cleanup;
  }
  if (build_album_skeleton(&state, err, sizeof(err)) != 0) {
    (void)string_buffer_append(error_out, err);
    goto cleanup;
  }
  if (probe_media(&state, error_out) != 0) {
    goto cleanup;
  }
  if (populate_output_manifest(&state, err, sizeof(err)) != 0) {
    (void)string_buffer_append(error_out, err);
    goto cleanup;
  }
  if (prepare_output_dir(&state, err, sizeof(err)) != 0) {
    (void)string_buffer_append(error_out, err);
    goto cleanup;
  }
  if (generate_derivatives(&state, error_out) != 0) {
    goto cleanup;
  }
  if (render_album_pages(&state, error_out) != 0) {
    goto cleanup;
  }
  if (write_generated_site(&state, err, sizeof(err)) != 0) {
    (void)string_buffer_append(error_out, err);
    goto cleanup;
  }

  build_verbose(&state, "build complete");
  rc = 0;

cleanup:
  build_state_free(&state);
  return rc;
}

// The `vfprintf` and `fputc` calls run on the main thread between phases, so they do not need the
// `flockfile`/`funlockfile` pair that `run_one_job` in `job.c` uses. That is temporal separation,
// not a guarantee. `pool_run` joins every worker before the next status line prints, so a progress
// line cannot land between a status line and its newline. Add the lock if anything ever writes
// `stderr` while a pool is running.
//
// Both writes are unchecked, and nothing calls `ferror(stderr)`. These lines are status, not
// diagnostics, and a build that otherwise succeeded should not fail because `stderr` was a closed
// pipe. The cost is that an `EPIPE` here goes unnoticed, unlike `gallery_config_print`, which
// reports it.
static void build_verbose(const struct BuildState* state, const char* fmt, ...) {
  if (!state->is_verbose) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  (void)vfprintf(stderr, fmt, ap);
  va_end(ap);
  (void)fputc('\n', stderr);
}

static void build_state_init(struct BuildState* state) {
  // This is one literal, so a field added to `BuildState` cannot be left out of its defaults. The
  // zeroed path lists and manifest are already their initialized state. `gallery_config_init` does
  // more: it installs the optional-key defaults, which `gallery_config_load` leaves untouched when
  // a key is absent.
  *state = (struct BuildState){0};
  gallery_config_init(&state->gallery_config);
}

static void build_state_free(struct BuildState* state) {
  if (state->rendered_pages != NULL) {
    for (size_t i = 0; i < state->album_count; i++) {
      free(state->rendered_pages[i].html);
    }
  }
  free(state->rendered_pages);
  album_scanner_free_video_frames(state->video_frame_paths, state->media_count);
  free(state->video_frame_paths);
  album_scanner_free_skeleton(state->albums, state->album_count, state->media);
  manifest_free(&state->manifest);
  path_list_free(&state->static_paths);
  path_list_free(&state->source_paths);
  gallery_config_free(&state->gallery_config);
  *state = (struct BuildState){0};
}

static int load_build_inputs(struct BuildState* state, char* err, size_t err_len) {
  build_verbose(state, "loading config");
  if (gallery_config_load(&state->gallery_config, GALLERY_CONFIG_PATH_DEFAULT, err, err_len) != 0) {
    return -1;
  }
  if (require_read_roots(state, err, err_len) != 0) {
    return -1;
  }
  if (manifest_builder_check_output_dir(&state->gallery_config, err, err_len) != 0) {
    return -1;
  }

  build_verbose(state, "discovering media");
  static const char* const media_suffixes[] = {".jpg", ".jpeg", ".mp4"};
  char reason[FS_REASON_SIZE];
  if (fs_list_files_with_suffixes(&state->source_paths, state->gallery_config.input_dir,
                                  state->gallery_config.output_dir, media_suffixes,
                                  sizeof(media_suffixes) / sizeof(media_suffixes[0]), true, reason,
                                  sizeof(reason)) != 0) {
    // The reason names the directory or entry that failed, which is more precise than the
    // configured root, so the root is not repeated here.
    return error_report(err, err_len, "failed to list source media: %s", reason);
  }
  build_verbose(state, "discovering static files");
  // An absent `static_dir` leaves `static_paths` empty rather than failing the build. A gallery
  // that ships no stylesheet is a supported configuration, and the walk stats its root before
  // descending, so calling it here would turn "no static files" into a failed build.
  // `require_read_roots` skips the same directory on the same condition.
  static const char* const static_suffixes[] = {""};
  if (fs_path_exists(state->gallery_config.static_dir) &&
      fs_list_files_with_suffixes(&state->static_paths, state->gallery_config.static_dir,
                                  state->gallery_config.output_dir, static_suffixes, 1, false,
                                  reason, sizeof(reason)) != 0) {
    return error_report(err, err_len, "failed to list static files: %s", reason);
  }
  if (require_video_tools(&state->source_paths, err, err_len) != 0) {
    return -1;
  }
  return 0;
}

static int require_read_roots(const struct BuildState* state, char* err, size_t err_len) {
  const struct ReadRoot roots[] = {
      {state->gallery_config.input_dir, "input_dir", false},
      {state->gallery_config.templates_dir, "templates_dir", false},
      {state->gallery_config.static_dir, "static_dir", true},
  };
  char reason[FS_REASON_SIZE];
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    // Two questions in order, because the second presupposes the first answer.
    //
    // Is it there at all? Only an optional root may answer no, and for that root absence is the
    // expected case rather than a failure, so the predicate is the right tool: it reports nothing.
    //
    // Is it usable? `fs_require_dir` answers with a cause, which absence needs and the predicate
    // cannot give. `ENOENT` and `EACCES` call for different fixes, and nothing after this point
    // would report either, because the loop returns here.
    if (roots[i].is_optional && !fs_path_exists(roots[i].path)) {
      continue;
    }
    if (fs_require_dir(roots[i].path, reason, sizeof(reason)) != 0) {
      return error_report(err, err_len, "failed to resolve config directory '%s': %s", roots[i].key,
                          reason);
    }
  }
  return 0;
}

static int require_video_tools(const struct PathList* source_paths, char* err, size_t err_len) {
  bool has_video = false;
  for (size_t i = 0; i < source_paths->count; i++) {
    has_video = has_video || media_item_is_video_path(source_paths->items[i]);
  }
  if (!has_video) {
    return 0;
  }

  const char* const ffmpeg_argv[] = {"ffmpeg", "-version", NULL};
  const char* const ffprobe_argv[] = {"ffprobe", "-version", NULL};
  char reason[ERROR_MESSAGE_SIZE];
  if (proc_run(ffmpeg_argv, NULL, 0, reason, sizeof(reason)) != 0) {
    return error_report(err, err_len, "video sources require ffmpeg: %s", reason);
  }
  if (proc_run(ffprobe_argv, NULL, 0, reason, sizeof(reason)) != 0) {
    return error_report(err, err_len, "video sources require ffprobe: %s", reason);
  }
  return 0;
}

static int build_album_skeleton(struct BuildState* state, char* err, size_t err_len) {
  build_verbose(state, "planning albums");
  if (album_scanner_build_skeleton(&state->gallery_config, &state->source_paths, &state->albums,
                                   &state->album_count, &state->media, &state->media_count, err,
                                   err_len) != 0) {
    return -1;
  }
  state->video_frame_paths = calloc(state->media_count, sizeof(*state->video_frame_paths));
  if (state->video_frame_paths == NULL && state->media_count > 0) {
    return error_report(err, err_len, "out of memory allocating video frame slots");
  }
  state->rendered_pages = calloc(state->album_count, sizeof(*state->rendered_pages));
  if (state->rendered_pages == NULL && state->album_count > 0) {
    return error_report(err, err_len, "out of memory allocating rendered album slots");
  }
  return 0;
}

static int probe_media(struct BuildState* state, struct StringBuffer* error_out) {
  if (state->media_count == 0) {
    return 0;
  }
  build_verbose(state, "probing media, workers: %zu", state->worker_count);
  return album_scanner_probe_media(&state->gallery_config, state->media, state->media_count,
                                   state->video_frame_paths, state->worker_count, state->is_verbose,
                                   error_out);
}

static int populate_output_manifest(struct BuildState* state, char* err, size_t err_len) {
  build_verbose(state, "building output manifest");
  // This runs ahead of the derivative and page renders, so a duplicate output path fails before
  // rendering is wasted. It also runs ahead of every write, `output_dir` itself included, so an
  // output aimed at one of this build's own inputs is refused before it can destroy the file.
  //
  // C has no implicit qualification conversion for pointer-to-pointer, so handing the album array
  // to a read-only callee needs this cast spelled out. It recurs at all three such call sites in
  // this file. It only adds `const`, at both levels, and the deep `const` is a thread-safety
  // requirement. A `TemplateContext` with only `const` pointers prevents one render job from
  // allocating into another album's arena.
  return manifest_builder_populate(&state->manifest, &state->gallery_config,
                                   GALLERY_CONFIG_PATH_DEFAULT, &state->source_paths,
                                   &state->static_paths, (const struct Album* const*)state->albums,
                                   state->album_count, err, err_len);
}

static int prepare_output_dir(const struct BuildState* state, char* err, size_t err_len) {
  char reason[FS_REASON_SIZE];
  if (fs_mkdir_p(state->gallery_config.output_dir, reason, sizeof(reason)) != 0) {
    // The reason names the component that failed, which is more precise than the configured root,
    // so the root is not repeated here.
    return error_report(err, err_len, "failed to prepare output directory: %s", reason);
  }
  return 0;
}

static int generate_derivatives(struct BuildState* state, struct StringBuffer* error_out) {
  if (state->media_count == 0) {
    return 0;
  }
  build_verbose(state, "generating derivatives, workers: %zu", state->worker_count);
  return derivative_renderer_generate(&state->gallery_config, state->media, state->media_count,
                                      state->video_frame_paths, state->worker_count,
                                      state->is_verbose, error_out);
}

static int render_album_pages(struct BuildState* state, struct StringBuffer* error_out) {
  build_verbose(state, "rendering albums, workers: %zu", state->worker_count);
  return page_renderer_render_pages(
      &state->gallery_config, (const struct Album* const*)state->albums, state->album_count,
      state->worker_count, state->is_verbose, state->rendered_pages, error_out);
}

static int write_generated_site(struct BuildState* state, char* err, size_t err_len) {
  build_verbose(state, "writing album pages");
  if (site_writer_write_album_pages((const struct Album* const*)state->albums, state->album_count,
                                    state->rendered_pages, err, err_len) != 0) {
    return -1;
  }
  build_verbose(state, "rendering aggregate templates");
  if (site_writer_write_aggregates(&state->gallery_config, state->albums[0], err, err_len) != 0) {
    return -1;
  }
  build_verbose(state, "copying static files");
  return site_writer_copy_static_files(&state->gallery_config, &state->static_paths, err, err_len);
}
