#include "app/cmd_build.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "build/album_scanner.h"
#include "build/derivative_renderer.h"
#include "build/job.h"
#include "build/manifest_builder.h"
#include "build/output_pruner.h"
#include "build/output_writer.h"
#include "build/page_renderer.h"
#include "build/site_writer.h"
#include "build/template.h"
#include "core/error.h"
#include "core/path_list.h"
#include "domain/album.h"
#include "domain/gallery_config.h"
#include "domain/manifest.h"
#include "domain/media_item.h"
#include "formats/image.h"
#include "runtime/memory.h"
#include "runtime/os.h"
#include "runtime/pool.h"
#include "runtime/proc.h"
#include "runtime/video.h"
#include "runtime/walk.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** Bytes in a mebibyte, the unit of the sizes below. */
#define BUILD_MIB ((ptrdiff_t)1024 * 1024)

// Each thread has a perm and a scratch arena of the sizes below, and the build has one log. A size
// costs address space only, because pages are committed as an arena fills, and a build that
// outgrows an arena ends in `os_oom`.
_Static_assert(PTRDIFF_MAX > 0xffffffff, "the arenas below need a 64-bit address space");

/**
 * Bytes of the build's log, and of each worker's share of the job logs, which hold the report: the
 * configuration and output-path errors and every failed job's messages.
 */
#define BUILD_LOG_LEN (256 * BUILD_MIB)

/**
 * Bytes of each thread's perm, which holds the main thread's paths, skeleton, frame slots and
 * manifest, or every album page a worker renders until the write phase, eight pages at
 * `RENDER_OUTPUT_LEN_MAX` in buffers of up to twice their length.
 */
#define BUILD_PERM_LEN (16 * (ptrdiff_t)RENDER_OUTPUT_LEN_MAX)

/**
 * Bytes of each thread's scratch, which holds one job's temporaries, at most a source of up to
 * `IMAGE_INPUT_LEN_MAX` beside one encoded derivative of up to `IMAGE_ENCODED_LEN_MAX`, or one page
 * render.
 */
#define BUILD_SCRATCH_LEN (3 * 1024 * BUILD_MIB)
_Static_assert(BUILD_SCRATCH_LEN > (ptrdiff_t)IMAGE_INPUT_LEN_MAX + IMAGE_ENCODED_LEN_MAX,
               "a worker's scratch must hold a source beside its encoded derivative");

/** Most workers the build starts for each online CPU, however many are requested. */
enum { BUILD_WORKER_PER_CPU_MAX = 4 };

/** One configured directory the build reads, which must be a usable directory when present. */
struct ReadRoot {
  /** Configured path, already slash-trimmed by the config load. */
  struct Str path;

  /** Config key naming `path`, used by every diagnostic so the user knows what to edit. */
  const char* key;

  /** Whether a gallery may legitimately not have this directory. */
  bool is_optional;

  /** Whether the config load rejected `key`, so `path` is its default and is not checked. */
  bool is_rejected;
};

/** The build's one reservation and the arenas `cmd_build_run` splits it into. */
struct BuildMemory {
  /** The whole reservation, which the build releases at its end. */
  struct Arena block;

  /** Workers the reservation has arenas for, at most the count asked for. */
  ptrdiff_t worker_count;

  /** Arena of the build's log, which no other code allocates from. */
  struct Arena log_arena;

  /** The main thread's perm. */
  struct Arena perm;

  /** The main thread's scratch, with room for one log share per worker and one more. */
  struct Arena scratch;

  /** One perm and one scratch per worker, in `perm`. */
  struct JobArenas* worker_arenas;
};

/** Mutable state owned by one `cmd_build_execute` invocation. */
struct BuildState {
  /** Host every phase reads from, writes to and prints on. */
  struct Os* os;

  /**
   * The main thread's perm, the caller's. It receives the configuration, the source and static
   * paths, the skeleton, the frame slots and the manifest.
   */
  struct Arena perm;

  /**
   * The main thread's scratch, the caller's. Each phase receives a copy, so nothing a phase
   * allocates here outlives it, and a parallel phase's copy holds its job logs.
   */
  struct Arena scratch;

  /**
   * One perm and one scratch per worker, `worker_count` of them. A worker's perm holds the album
   * pages it rendered, so it must not be reset before the write phase reads them.
   */
  struct JobArenas* worker_arenas;

  /** Loaded gallery configuration. Its values live in `perm`. */
  struct GalleryConfig gallery_config;

  /** Discovered media source paths below `input_dir`, sorted by the walk that produced them. */
  struct PathList source_paths;

  /** Discovered static file paths below `static_dir`, left empty when that directory is absent. */
  struct PathList static_paths;

  /** Intended output paths for this build, populated before any file is written. */
  struct Manifest manifest;

  /** Writer every change under `output_dir` goes through, which marks `manifest`. */
  struct OutputWriter writer;

  /** Scanned albums sorted by `source_dir`, with the root album at index `0`, in `perm`. */
  struct Album** albums;

  /** Number of albums in `albums`, and therefore the number of `rendered_pages` slots. */
  ptrdiff_t album_count;

  /** Every media item of every album, in source order, in `perm`. */
  struct MediaItem** media;

  /** Number of media items in `media`, and therefore the number of `video_frames` slots. */
  ptrdiff_t media_count;

  /**
   * One frame slot per media item, whose path the build allocates in `perm` for a video before the
   * probe phase. The probe creates the frame, the derivative phase removes it, and the build
   * removes any it left behind at its end.
   */
  struct VideoFrame* video_frames;

  /**
   * One slot per album, holding that album's rendered page. A slot's `html` stays `NULL` until the
   * page render fills it, in the perm of the worker that rendered it.
   */
  struct RenderedPage* rendered_pages;

  /** Number of video sources among `source_paths`. */
  ptrdiff_t video_count;

  /** Most worker threads a parallel phase runs, each with its arenas in `worker_arenas`. */
  ptrdiff_t worker_count;

  /** Whether phase progress and status messages are printed to `stderr`. */
  bool is_verbose;
};

/**
 * @brief Resolves the worker count: the requested one, or the CPU count when unset, and at most
 *        `BUILD_WORKER_PER_CPU_MAX` per online CPU, because a worker waiting for a CPU only adds a
 *        thread and its arenas.
 *
 * @param count_requested Workers the command line asks for, or `0` for the CPU count.
 * @return The number of workers the build reserves arenas for.
 */
static ptrdiff_t cmd_build_run_worker_count(ptrdiff_t count_requested);

/**
 * @brief Reserves the build's memory in one block: the log, and a perm and a scratch for the main
 *        thread and for each worker.
 *
 * A reservation the system refuses halves the worker count, down to one, and only one worker's
 * reservation is halved, every arena with it, as `arena_reserve_halving` describes.
 *
 * @param worker_count Workers to reserve arenas for. Must be positive.
 * @return The reservation, its arenas, and the worker count they are for.
 */
static struct BuildMemory cmd_build_run_reserve(ptrdiff_t worker_count);

/**
 * @brief Initializes a build state over the caller's arenas.
 *
 * @param state         Build state to prepare. Must not be `NULL`.
 * @param options       Build knobs, whose worker count is positive. Must not be `NULL`.
 * @param worker_arenas One perm and one scratch per worker. Must not be `NULL`.
 * @param os            Host the build runs against. Must not be `NULL`.
 * @param perm          The main thread's perm.
 * @param scratch       The main thread's scratch.
 */
static void build_state_init(struct BuildState* state,
                             const struct BuildOptions* options,
                             struct JobArenas* worker_arenas,
                             struct Os* os,
                             struct Arena perm,
                             struct Arena scratch) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Loads `fram.toml`, rejects an `output_dir` inside an input tree, and discovers media and
 *        static files.
 *
 * The `output_dir` check runs before either walk, and both walks leave `output_dir` out, so a file
 * an earlier build generated is never read back as a source.
 *
 * A config that was read and parsed but holds a bad key has logged it and kept that key's default,
 * so the checks after it still run against it. A rejected directory key is the exception: its
 * default is a directory the user never chose, so nothing checks or walks it, and the load stops
 * once it has checked the other read roots.
 *
 * @param state Build state that receives the configuration, media source paths, and static file
 *              paths. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` when there are sources to check, or `-1` on a configuration that cannot be read, a
 *         rejected directory key, a directory, output directory, listing, or missing-tool failure.
 */
static int load_build_inputs(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Lists the media sources below `input_dir` and counts the videos.
 *
 * One walk lists every media suffix.
 *
 * @param state Build state that receives the source paths and the video count. Must not be `NULL`.
 * @param log   Log that receives the failure, which names the path the walk failed on. Must not be
 *              `NULL`.
 * @return `0` on success, or `-1` on a directory or entry failure.
 */
static int load_build_inputs_media(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Lists the static files below `static_dir`, which a gallery may leave out.
 *
 * @param state Build state that receives the static file paths. Must not be `NULL`.
 * @param log   Log that receives the failure, which names the path the walk failed on. Must not be
 *              `NULL`.
 * @return `0` when `static_dir` is absent or was listed, or `-1` on a directory or entry failure.
 */
static int load_build_inputs_static(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Requires every configured read root to be a usable directory.
 *
 * Walks the `ReadRoot` spec table, so each diagnostic names the config key the user has to edit. An
 * optional root that is not there is skipped, and any other root that cannot be resolved is
 * reported with its cause, and every root is checked except one whose key the config load rejected,
 * which it has reported already.
 *
 * @param state Build state supplying the configured read roots. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` when every present root is a usable directory, or `-1` when one cannot be resolved or
 *         is not a directory.
 */
static int load_build_inputs_require_read_roots(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Requires `ffmpeg` and `ffprobe` to be runnable when any source is a video.
 *
 * A gallery of images alone needs neither tool, so the probe runs only once a video source has been
 * discovered. Running both here reports a missing tool once, before the parallel probe and
 * derivative phases would report it per video. Both tools are checked.
 *
 * @param state Build state supplying the video count and the scratch arena. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` when no source is a video or both tools ran, or `-1` when either could not be run.
 */
static int load_build_inputs_require_video_tools(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Runs the phases from the skeleton to the prune, and in a dry run the report.
 *
 * Probes every media item and checks every output path even after a probe fails, then stops before
 * the first write when `log` holds any error, an earlier phase's included. A skeleton that cannot
 * be planned, a path-limit failure included, stops the build at once, because the later phases read
 * it. Removes every video frame the phases left behind before it returns, on success and on
 * failure.
 *
 * @param state Build state whose inputs are loaded. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` on a complete build, or `-1` on any error.
 */
static int run_build_phases(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Builds the album and media skeleton and allocates one result slot per media item and per
 *        album.
 *
 * @param state Build state that receives the albums, the flat media array, the video frame slots,
 *              and the rendered page slots. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` on a scan or path-limit failure.
 */
static int build_album_skeleton(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Probes every media item's metadata, dispatching jobs across worker threads.
 *
 * A gallery with no media skips the phase, so it prints no status line and starts no pool.
 *
 * @param state Build state holding the media items and video frame slots. Must not be `NULL`.
 * @param log   Log that receives the collected probe errors. Must not be `NULL`.
 * @return `0` when every job succeeded or there is no media, or `-1` when any probe job failed.
 */
static int probe_media(struct BuildState* state, struct Log* log) __attribute__((nonnull(1, 2)));

/**
 * @brief Records every intended output path, rejecting a collision before anything is written.
 *
 * @param state Build state whose manifest is populated from the configuration, the discovered
 *              inputs, and the scanned albums. Must not be `NULL`.
 * @param log   Log that receives every collision. Must not be `NULL`.
 * @return `0` when every output path is unique and names no build input, or `-1` on a collision or
 *         an input overwrite.
 */
static int populate_output_manifest(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Creates the output directory once the manifest has accepted every output path.
 *
 * Nothing before this phase writes, so a build the manifest or an earlier phase refuses leaves the
 * filesystem as it found it, without even an empty `output_dir`. A dry run creates nothing.
 *
 * @param state Build state supplying `output_dir`. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a component of `output_dir` cannot be created.
 */
static int prepare_output_dir(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Generates every media item's derivatives, dispatching jobs across worker threads.
 *
 * A gallery with no media skips the phase, so it prints no status line and starts no pool.
 *
 * @param state Build state holding the probed media items and video frame slots. Must not be
 *              `NULL`.
 * @param log   Log that receives the collected derivative errors. Must not be `NULL`.
 * @return `0` when every job succeeded or there is no media, or `-1` when any derivative job
 *         failed.
 */
static int generate_derivatives(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Renders every album's page, dispatching jobs across worker threads.
 *
 * A gallery always has its root album, so this phase never runs empty.
 *
 * @param state Build state holding the scanned albums and rendered page slots. Must not be `NULL`.
 * @param log   Log that receives the collected render errors. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when any render job failed.
 */
static int render_album_pages(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the album pages, the aggregate template outputs, and the copied static files.
 *
 * @param state Build state holding the rendered pages, the scanned albums, and the static file
 *              paths. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` when every output was written, or `-1` on the first render or write failure.
 */
static int write_generated_site(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Removes every entry below `output_dir` that this build did not write.
 *
 * Run it only once every output is written, so a failed build removes nothing.
 *
 * @param state Build state holding the manifest and the configuration. Must not be `NULL`.
 * @param log   Log that receives every failure. Must not be `NULL`.
 * @return `0` when every stale entry was removed, or `-1` when one could not be listed or removed.
 */
static int prune_stale_outputs(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Prints to `stdout` what a dry run would have written, kept, and removed, as
 *        `output_writer_report` lists it.
 *
 * @param state Build state whose writer has run the dry run. Must not be `NULL`.
 * @param log   Log that receives every output path no site marked, or the failed print. Must not be
 *              `NULL`.
 * @return `0` once the listing is printed, or `-1` when an output path was neither written nor kept
 *         or the print fails.
 */
static int report_dry_run(struct BuildState* state, struct Log* log) __attribute__((nonnull(1, 2)));

/**
 * @brief Prints a formatted status line to `stderr` when the build is verbose.
 *
 * The line is formatted in a copy of the main thread's scratch arena and printed in one `os_print`
 * call.
 *
 * @param state  Build state whose verbosity gates the output. Must not be `NULL`.
 * @param format `printf` format of the line, without the newline. Must not be `NULL`.
 * @param ...    Arguments that the format converts.
 */
static void build_verbose(const struct BuildState* state, const char* format, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

enum ExitCode cmd_build_run(const struct BuildOptions* options, struct Os* os) {
  struct BuildMemory memory =
      cmd_build_run_reserve(cmd_build_run_worker_count(options->worker_count));
  // Every option is carried as it was asked for, except the worker count the memory allows.
  struct BuildOptions resolved = *options;
  resolved.worker_count = memory.worker_count;
  struct Log log = {.buf = {.arena = &memory.log_arena}};
  memory_oom_set_log(&log);
  enum ExitCode rc = EXIT_CODE_OK;
  if (cmd_build_execute(&resolved, memory.worker_arenas, os, memory.perm, memory.scratch, &log) !=
      0) {
    // The log is the whole report, each error on its own line.
    (void)os_print(os, 2, buf_str(&log.buf));
    rc = EXIT_CODE_FAILURE;
  }
  // The names point into memory released below.
  memory_oom_set_log(NULL);
  memory_oom_set_job((struct Str){0}, NULL);
  arena_release(memory.block);
  return rc;
}

int cmd_build_execute(const struct BuildOptions* options,
                      struct JobArenas* worker_arenas,
                      struct Os* os,
                      struct Arena perm,
                      struct Arena scratch,
                      struct Log* log) {
  struct BuildState state;
  build_state_init(&state, options, worker_arenas, os, perm, scratch);

  // Until the first write, a phase that fails still lets the next one run whenever that one has
  // something to check, so one run reports a bad config key together with every path-limit failure
  // in the skeleton, or, once the skeleton is planned, with every source that cannot be probed and
  // every output collision. Only a failure that leaves nothing to check stops early: a config that
  // cannot be read, a root that cannot be walked, a missing video tool, and a skeleton error, a
  // path-limit failure included, because the later phases read the skeleton.
  if (load_build_inputs(&state, log) != 0) {
    return -1;
  }
  return run_build_phases(&state, log);
}

static ptrdiff_t cmd_build_run_worker_count(ptrdiff_t count_requested) {
  const ptrdiff_t cpu_count = pool_resolve_worker_count();
  const ptrdiff_t count = count_requested != 0 ? count_requested : cpu_count;
  const ptrdiff_t count_max = BUILD_WORKER_PER_CPU_MAX * cpu_count;
  return count < count_max ? count : count_max;
}

static struct BuildMemory cmd_build_run_reserve(ptrdiff_t worker_count) {
  // The main thread's scratch also holds a parallel phase's job logs, which `job_run` splits into
  // one share per worker and one for the pool's bookkeeping. So the main thread has the log, its
  // perm, its scratch and the pool's share, and each worker adds its perm, scratch and share.
  const ptrdiff_t main_len = BUILD_LOG_LEN + BUILD_PERM_LEN + BUILD_SCRATCH_LEN + BUILD_LOG_LEN;
  const ptrdiff_t worker_len = BUILD_PERM_LEN + BUILD_SCRATCH_LEN + BUILD_LOG_LEN;
  struct BuildMemory memory = {0};
  // A limit costs workers before it costs room.
  while (worker_count > 1 &&
         arena_reserve(main_len + worker_len * worker_count, &memory.block) != 0) {
    worker_count /= 2;
  }
  const int shift =
      worker_count > 1 ? 0 : arena_reserve_halving(main_len + worker_len, &memory.block);
  memory.worker_count = worker_count;
  struct Arena rest = memory.block;
  memory.log_arena = arena_split(&rest, BUILD_LOG_LEN >> shift);
  memory.perm = arena_split(&rest, BUILD_PERM_LEN >> shift);
  memory.scratch =
      arena_split(&rest, (BUILD_SCRATCH_LEN + BUILD_LOG_LEN * (worker_count + 1)) >> shift);
  memory.worker_arenas = arena_new(&memory.perm, worker_count, struct JobArenas);
  for (ptrdiff_t i = 0; i < worker_count; i++) {
    memory.worker_arenas[i].perm = arena_split(&rest, BUILD_PERM_LEN >> shift);
    memory.worker_arenas[i].scratch = arena_split(&rest, BUILD_SCRATCH_LEN >> shift);
  }
  return memory;
}

static void build_state_init(struct BuildState* state,
                             const struct BuildOptions* options,
                             struct JobArenas* worker_arenas,
                             struct Os* os,
                             struct Arena perm,
                             struct Arena scratch) {
  // This is one literal, so a field added to `BuildState` cannot be left out of its defaults. The
  // zeroed path lists and manifest are already their empty state. `gallery_config_init` does more:
  // it installs the optional-key defaults, which `gallery_config_load` leaves untouched when a key
  // is absent.
  *state = (struct BuildState){
      .os = os,
      .perm = perm,
      .scratch = scratch,
      .worker_arenas = worker_arenas,
      .writer =
          {
              .manifest = &state->manifest,
              .os = os,
              .is_dry_run = options->is_dry_run,
              .perm = &state->perm,
          },
      .worker_count = options->worker_count,
      .is_verbose = options->is_verbose,
  };
  gallery_config_init(&state->gallery_config);
}

static int load_build_inputs(struct BuildState* state, struct Log* log) {
  build_verbose(state, "loading config");
  if (gallery_config_load(&state->gallery_config, GALLERY_CONFIG_PATH_DEFAULT, state->os,
                          &state->perm, state->scratch, log) != 0) {
    return -1;
  }
  // A rejected directory key holds a default the user never chose, so the build stops before the
  // checks and the walks that would report that default as a second error.
  const struct GalleryConfig* gallery_config = &state->gallery_config;
  if (load_build_inputs_require_read_roots(state, log) != 0 ||
      gallery_config->is_input_dir_rejected || gallery_config->is_output_dir_rejected ||
      gallery_config->is_templates_dir_rejected || gallery_config->is_static_dir_rejected) {
    return -1;
  }
  if (manifest_builder_check_output_dir(&state->gallery_config, GALLERY_CONFIG_PATH_DEFAULT,
                                        state->os, state->scratch, log) != 0) {
    return -1;
  }

  build_verbose(state, "discovering media");
  if (load_build_inputs_media(state, log) != 0) {
    return -1;
  }
  build_verbose(state, "discovering static files");
  if (load_build_inputs_static(state, log) != 0) {
    return -1;
  }
  return load_build_inputs_require_video_tools(state, log);
}

static int load_build_inputs_media(struct BuildState* state, struct Log* log) {
  static const struct Str media_suffixes[] = {STR_INIT(".jpg"), STR_INIT(".jpeg"),
                                              STR_INIT(".mp4")};
  // A failure names the directory or entry that failed, which is more precise than the configured
  // root, so the root is not repeated.
  if (walk_files_with_suffixes(
          &state->source_paths, state->gallery_config.input_dir, state->gallery_config.output_dir,
          media_suffixes, (ptrdiff_t)(sizeof(media_suffixes) / sizeof(media_suffixes[0])), true,
          state->os, &state->perm, state->scratch, S("failed to list source media"), log) != 0) {
    return -1;
  }
  for (ptrdiff_t i = 0; i < state->source_paths.count; i++) {
    state->video_count += media_item_is_video_path(state->source_paths.items[i]);
  }
  return 0;
}

static int load_build_inputs_static(struct BuildState* state, struct Log* log) {
  // An absent `static_dir` leaves `static_paths` empty rather than failing the build. A gallery
  // that ships no stylesheet is a supported configuration, and the walk stats its root before
  // descending, so calling it here would turn "no static files" into a failed build.
  // `load_build_inputs_require_read_roots` skips the same directory on the same condition. A
  // failure names the directory or entry that failed, which is more precise than the configured
  // root, so the root is not repeated.
  struct OsStat st;
  if (os_stat(state->os, state->gallery_config.static_dir, &st) != 0 && errno == ENOENT) {
    return 0;
  }
  static const struct Str static_suffixes[] = {STR_INIT("")};
  return walk_files_with_suffixes(&state->static_paths, state->gallery_config.static_dir,
                                  state->gallery_config.output_dir, static_suffixes, 1, false,
                                  state->os, &state->perm, state->scratch,
                                  S("failed to list static files"), log);
}

static int load_build_inputs_require_read_roots(const struct BuildState* state, struct Log* log) {
  const struct ReadRoot roots[] = {
      {state->gallery_config.input_dir, "input_dir", false,
       state->gallery_config.is_input_dir_rejected},
      {state->gallery_config.templates_dir, "templates_dir", false,
       state->gallery_config.is_templates_dir_rejected},
      {state->gallery_config.static_dir, "static_dir", true,
       state->gallery_config.is_static_dir_rejected},
  };
  int rc = 0;
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(roots) / sizeof(roots[0])); i++) {
    // A root whose key the config load rejected holds its default, and the load has reported it.
    // Any other root gets two questions in order, because the second presupposes the first answer.
    //
    // Is it there at all? Only an optional root may answer no, and for that root absence is the
    // expected case rather than a failure, so it is skipped without a message. Only `ENOENT`
    // answers no. A root that cannot be inspected, such as one below a directory the user may not
    // search, may well be there, so it is reported like any other.
    //
    // Is it usable? The message carries the cause. `ENOENT` and `EACCES` call for different fixes,
    // and nothing after this point would report either, because the build stops once a root is
    // unusable. The cause carries no verb of its own, so the operation names what was being
    // resolved, as in
    // `failed to resolve config directory 'input_dir': No such file or directory ('photos')`.
    if (roots[i].is_rejected) {
      continue;
    }
    struct OsStat st;
    if (os_stat(state->os, roots[i].path, &st) != 0) {
      if (roots[i].is_optional && errno == ENOENT) {
        continue;
      }
      char message[ERROR_SYSTEM_MESSAGE_SIZE];
      rc = log_error(log, "failed to resolve config directory '%s': %s ('%.*s')", roots[i].key,
                     error_system_message(message, sizeof(message), errno), STR_FMT(roots[i].path));
    } else if (!st.is_dir) {
      rc = log_error(log, "failed to resolve config directory '%s': not a directory ('%.*s')",
                     roots[i].key, STR_FMT(roots[i].path));
    }
  }
  return rc;
}

static int load_build_inputs_require_video_tools(const struct BuildState* state, struct Log* log) {
  if (state->video_count == 0) {
    return 0;
  }

  const char* const ffmpeg_argv[] = {"ffmpeg", "-version", NULL};
  const char* const ffprobe_argv[] = {"ffprobe", "-version", NULL};
  // Each run works in its own copy of the scratch arena, which the next one reuses.
  struct Arena ffmpeg_scratch = state->scratch;
  int rc = proc_run(ffmpeg_argv, &ffmpeg_scratch, NULL, S("video sources require ffmpeg"),
                    (struct Str){0}, log);
  struct Arena ffprobe_scratch = state->scratch;
  if (proc_run(ffprobe_argv, &ffprobe_scratch, NULL, S("video sources require ffprobe"),
               (struct Str){0}, log) != 0) {
    rc = -1;
  }
  return rc;
}

static int run_build_phases(struct BuildState* state, struct Log* log) {
  // The order below follows the data: the probe records each item's derivative dimensions, which
  // the derivative and page phases read, and extracts each video's still frame, which the
  // derivative phase reads. Every source has to be probed before any derivative is generated or any
  // page is rendered. Output paths do not depend on probed dimensions, because a derivative path
  // names the recipe and not the size, so the manifest checks every output even after a probe
  // failed.
  if (build_album_skeleton(state, log) != 0) {
    return -1;
  }
  int rc = -1;
  (void)probe_media(state, log);
  (void)populate_output_manifest(state, log);
  if (log->errors > 0) {
    goto cleanup;
  }

  // Nothing before this point writes, so a build stopped above leaves the filesystem as it found
  // it. From here on a failure stops the phase it happens in.
  if (prepare_output_dir(state, log) != 0) {
    goto cleanup;
  }
  if (generate_derivatives(state, log) != 0 || render_album_pages(state, log) != 0) {
    goto cleanup;
  }
  if (write_generated_site(state, log) != 0 || prune_stale_outputs(state, log) != 0) {
    goto cleanup;
  }
  if (state->writer.is_dry_run && report_dry_run(state, log) != 0) {
    goto cleanup;
  }

  build_verbose(state, "build complete");
  rc = 0;

cleanup:
  album_scanner_remove_video_frames(state->video_frames, state->media_count);
  return rc;
}

static int build_album_skeleton(struct BuildState* state, struct Log* log) {
  build_verbose(state, "planning albums");
  if (album_scanner_build_skeleton(&state->gallery_config, &state->source_paths, &state->perm,
                                   state->scratch, &state->albums, &state->album_count,
                                   &state->media, &state->media_count, log) != 0) {
    return -1;
  }
  // The slots are allocated here, on the main thread, before either phase that uses them, so a job
  // only fills in its own slot and never allocates in the main thread's arenas.
  state->video_frames = arena_new(&state->perm, state->media_count, struct VideoFrame);
  for (ptrdiff_t i = 0; i < state->media_count; i++) {
    if (state->media[i]->kind == MEDIA_KIND_VIDEO) {
      video_frame_init(&state->video_frames[i], &state->perm);
    }
  }
  state->rendered_pages = arena_new(&state->perm, state->album_count, struct RenderedPage);
  return 0;
}

static int probe_media(struct BuildState* state, struct Log* log) {
  if (state->media_count == 0) {
    return 0;
  }
  build_verbose(state, "probing media, workers: %td", state->worker_count);
  return album_scanner_probe_media(&state->gallery_config, state->media, state->media_count,
                                   state->video_frames, state->worker_count, state->worker_arenas,
                                   state->is_verbose, state->os, state->scratch, log);
}

static int populate_output_manifest(struct BuildState* state, struct Log* log) {
  build_verbose(state, "building output manifest");
  // This runs ahead of the derivative and page renders, so a duplicate output path fails before
  // rendering is wasted. It also runs ahead of every write, `output_dir` itself included, so an
  // output aimed at one of this build's own inputs is refused before it can destroy the file.
  //
  // C has no implicit qualification conversion for pointer-to-pointer, so handing the album array
  // to a read-only callee needs this cast spelled out. It recurs at all three such call sites in
  // this file. It only adds `const`, at both levels, and the deep `const` is a thread-safety
  // requirement. A `TemplateContext` with only `const` pointers keeps one render job from writing
  // an album another job is reading.
  return manifest_builder_populate(
      &state->manifest, &state->gallery_config, GALLERY_CONFIG_PATH_DEFAULT, &state->source_paths,
      &state->static_paths, (const struct Album* const*)state->albums, state->album_count,
      state->os, &state->perm, state->scratch, log);
}

static int prepare_output_dir(const struct BuildState* state, struct Log* log) {
  // A failure names the component that failed, which is more precise than the configured root, so
  // the root is not repeated.
  return output_writer_mkdirs(&state->writer, state->gallery_config.output_dir,
                              S("failed to prepare output directory"), log);
}

static int generate_derivatives(struct BuildState* state, struct Log* log) {
  if (state->media_count == 0) {
    return 0;
  }
  build_verbose(state, "generating derivatives, workers: %td", state->worker_count);
  return derivative_renderer_generate(
      &state->writer, &state->gallery_config, state->media, state->media_count, state->video_frames,
      state->worker_count, state->worker_arenas, state->is_verbose, state->os, state->scratch, log);
}

static int render_album_pages(struct BuildState* state, struct Log* log) {
  build_verbose(state, "rendering albums, workers: %td", state->worker_count);
  return page_renderer_render_pages(&state->gallery_config,
                                    (const struct Album* const*)state->albums, state->album_count,
                                    state->worker_count, state->worker_arenas, state->is_verbose,
                                    state->os, state->scratch, state->rendered_pages, log);
}

static int write_generated_site(struct BuildState* state, struct Log* log) {
  build_verbose(state, "writing album pages");
  if (site_writer_write_album_pages(&state->writer, (const struct Album* const*)state->albums,
                                    state->album_count, state->rendered_pages, state->scratch,
                                    log) != 0) {
    return -1;
  }
  build_verbose(state, "rendering aggregate templates");
  if (site_writer_write_aggregates(&state->writer, &state->gallery_config, state->albums[0],
                                   state->os, state->perm, state->scratch, log) != 0) {
    return -1;
  }
  build_verbose(state, "copying static files");
  return site_writer_copy_static_files(&state->writer, &state->gallery_config, &state->static_paths,
                                       state->scratch, log);
}

static int prune_stale_outputs(struct BuildState* state, struct Log* log) {
  build_verbose(state, "removing stale outputs");
  ptrdiff_t removed_count = 0;
  const int rc = output_pruner_prune(&state->writer, &state->gallery_config, state->os,
                                     state->scratch, &removed_count, log);
  build_verbose(state, "removed stale outputs: %td", removed_count);
  return rc;
}

static int report_dry_run(struct BuildState* state, struct Log* log) {
  return output_writer_report(&state->writer, state->scratch, log);
}

// A status line runs on the main thread between phases, so it does not need the lock that
// `job_run_one_print_progress` in `job.c` holds around a progress line. It relies on that ordering
// rather than on the lock: `pool_run` joins every worker before the next status line prints, so a
// progress line cannot land in the middle of a status line. Take the same lock if anything ever
// prints while a pool is running.
//
// The print is unchecked. These lines are status, not diagnostics, and a build that otherwise
// succeeded should not fail because `stderr` was a closed pipe. The cost is that an `EPIPE` here
// goes unnoticed, unlike `gallery_config_print`, which reports it.
static void build_verbose(const struct BuildState* state, const char* format, ...) {
  if (!state->is_verbose) {
    return;
  }
  // The first pass measures the line and the second writes it, as `buf_printf` does. Starting the
  // list twice needs no `va_copy`, and forwarding it to `buf_vprintf` instead makes Clang's
  // analyzer report a false `va_end` on AArch64 Linux.
  va_list args;
  va_start(args, format);
  const int line_len = vsnprintf(NULL, 0, format, args);
  va_end(args);
  assert(line_len >= 0);
  struct Arena scratch = state->scratch;
  char* line = arena_new(&scratch, (ptrdiff_t)line_len + 1, char);
  va_start(args, format);
  (void)vsnprintf(line, (size_t)line_len + 1, format, args);
  va_end(args);
  // The terminator `vsnprintf` wrote becomes the newline, so the line prints in one call.
  line[line_len] = '\n';
  (void)os_print(state->os, 2, (struct Str){line, (ptrdiff_t)line_len + 1});
}
