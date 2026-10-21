#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/cmd_build.h"
#include "app/exit_code.h"
#include "build/job.h"
#include "build/test_jpeg.h"
#include "core/path_list.h"
#include "formats/image.h"
#include "os_memory.h"
#include "runtime/memory.h"
#include "runtime/pool.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// Every build here runs against the in-memory host, whose root is the working directory the build
// resolves `fram.toml` from, and which keeps what the build prints. No fixture holds a video, so no
// build here runs `ffprobe` or `ffmpeg`. The build that does is in `test_cmd_build_video.c`, on the
// POSIX host. The golden galleries build the real executable on the real filesystem.

/**
 * Bytes of the arenas `build_in_tiny_arena` gives a build, a quarter for each of the main thread's
 * and the worker's perm and scratch. Each is too small for the configuration the build loads.
 */
enum { TINY_ARENA_SIZE = 128 };

/** Config body every fixture shares. Derivatives stay small so encoding stays cheap. */
static const char* const GALLERY_CONFIG =
    "title = \"Test Gallery\"\n"
    "author = \"Tests\"\n"
    "input_dir = \"photos\"\n"
    "[derivatives.s]\nwidth = 8\nheight = 8\nquality = 70\ncrop = true\n"
    "[derivatives.m]\nwidth = 16\nheight = 16\nquality = 75\n";

/** Minimal album template. It renders without naming any derivative, so sizes stay free. */
static const char* const ALBUM_TEMPLATE =
    "<!doctype html><title>{{album.title}} - {{gallery.title}}</title>"
    "{{#has_media}}<ul>{{#media}}<li>{{title}}</li>{{/media}}</ul>{{/has_media}}";

/**
 * Album template for the large album, which lists each item the way a real gallery's template does:
 * its links, its derivatives' URLs and dimensions, and its title.
 */
static const char* const LARGE_ALBUM_TEMPLATE =
    "<!doctype html><title>{{album.title}} - {{gallery.title}}</title><main>"
    "{{#media}}<a class=\"item{{#is_image}} item--image{{/is_image}}"
    "{{#is_video}} item--video{{/is_video}}\" href=\"{{original_url}}\" download=\"{{title}}\" "
    "data-small-src=\"{{s.url}}\" data-small-width=\"{{s.width}}\" "
    "data-small-height=\"{{s.height}}\"><img src=\"{{m.url}}\" width=\"{{m.width}}\" "
    "height=\"{{m.height}}\" alt=\"{{title}}\"><span>{{title}}</span></a>{{/media}}</main>";

/**
 * @brief Writes one JPEG, creating its parent directories, as a fixture.
 *
 * The fixture encodes its own source rather than committing a binary file, so no test needs a
 * checked-in image and none needs `ffmpeg`.
 *
 * @param os        Host to write to. Must not be `NULL`.
 * @param file_path Terminated path of the JPEG. Must not be `NULL`.
 */
static void write_jpeg(struct Os* os, const char* file_path) {
  struct Arena arena = test_arena();
  struct Str jpeg = {0};
  if (TEST_CHECK(test_jpeg_encode(24, 12, &arena, &jpeg) == 0)) {
    os_memory_write_bytes(os, str_from_cstr(file_path), jpeg);
  }
}

/**
 * @brief Writes `fram.toml` and the album template.
 *
 * @param os           Host to write to. Must not be `NULL`.
 * @param config_extra Config lines placed before the shared body, or `NULL` for the body alone. It
 *                     must not repeat a key the body sets, because TOML rejects a duplicate key.
 */
static void write_gallery_fixture(struct Os* os, const char* config_extra) {
  os_memory_write(os, "templates/album.html", ALBUM_TEMPLATE);
  struct Arena arena = test_arena();
  struct Buf config = {.arena = &arena};
  if (config_extra != NULL) {
    buf_append(&config, str_from_cstr(config_extra));
  }
  buf_append(&config, str_from_cstr(GALLERY_CONFIG));
  os_memory_write(os, "fram.toml", buf_cstr(&config));
}

/**
 * @brief Executes a build with the given options in test arenas and collects its errors.
 *
 * @param options Build options passed to `cmd_build_execute`, whose worker count `0` stands for the
 *                CPU count, as on the command line. Must not be `NULL`.
 * @param os      Host the build runs against. Must not be `NULL`.
 * @param log     Log that receives every build error. Its arena must be none of the arenas the
 *                build allocates from, as `cmd_build_execute` requires. Must not be `NULL`.
 * @return `0` on success, or `-1` on build failure.
 */
static int execute_build_with_options(const struct BuildOptions* options,
                                      struct Os* os,
                                      struct Log* log) {
  struct BuildOptions resolved = *options;
  if (resolved.worker_count == 0) {
    resolved.worker_count = pool_resolve_worker_count();
  }
  struct Arena perm = test_arena();
  struct JobArenas* worker_arenas = arena_new(&perm, resolved.worker_count, struct JobArenas);
  for (ptrdiff_t i = 0; i < resolved.worker_count; i++) {
    worker_arenas[i] = (struct JobArenas){.perm = test_arena(), .scratch = test_arena()};
  }
  return cmd_build_execute(&resolved, worker_arenas, os, perm, test_arena(), log);
}

/**
 * @brief Executes a build with default options and collects its errors.
 *
 * @param os  Host the build runs against. Must not be `NULL`.
 * @param log Log that receives every build error. Must not be `NULL`.
 * @return `0` on success, or `-1` on build failure.
 */
static int execute_build(struct Os* os, struct Log* log) {
  const struct BuildOptions options = {0};
  return execute_build_with_options(&options, os, log);
}

/**
 * @brief Reports whether the regular files below `dir_path` are exactly `expected`, printing them
 *        when they are not.
 *
 * @param os             Host to inspect. Must not be `NULL`.
 * @param dir_path       Terminated directory to list. Must not be `NULL`.
 * @param expected       Array of `expected_count` terminated paths in byte order. Must not be
 *                       `NULL`.
 * @param expected_count Number of expected paths.
 * @return `true` when the files match.
 */
static bool is_file_list(struct Os* os,
                         const char* dir_path,
                         const char* const* expected,
                         ptrdiff_t expected_count) {
  const struct PathList files = os_memory_files(os, dir_path);
  bool is_equal = files.count == expected_count;
  for (ptrdiff_t i = 0; is_equal && i < expected_count; i++) {
    is_equal = str_eq(files.items[i], str_from_cstr(expected[i]));
  }
  TEST_CHECK_(is_equal, "'%s' holds exactly the expected files", dir_path);
  for (ptrdiff_t i = 0; !is_equal && i < files.count; i++) {
    TEST_MSG("found: '%.*s'", STR_FMT(files.items[i]));
  }
  return is_equal;
}

/**
 * @brief Reports whether `os_print` has written `line` as a whole line to `fd`, printing the stream
 *        when it has not.
 *
 * @param os   Host to inspect. Must not be `NULL`.
 * @param fd   `1` for standard output or `2` for standard error.
 * @param line Terminated line, without its newline. Must not be `NULL`.
 * @return `true` when one of the printed lines is `line`.
 */
static bool has_printed_line(struct Os* os, int fd, const char* line) {
  const struct Str printed = os_memory_printed(os, fd);
  bool is_printed = false;
  struct StrCut cut = {.tail = printed, .is_found = true};
  while (!is_printed && cut.is_found) {
    cut = str_cut(cut.tail, '\n');
    is_printed = str_eq(cut.head, str_from_cstr(line));
  }
  TEST_CHECK_(is_printed, "'%s' is a printed line", line);
  if (!is_printed) {
    TEST_MSG("printed: '%.*s'", STR_FMT(printed));
  }
  return is_printed;
}

/**
 * @brief Runs a one-worker build in `TINY_ARENA_SIZE` bytes of arenas. Run through `test_is_oom`,
 *        which calls it in a child process.
 *
 * @param userdata Host the build runs against, as a `struct Os*`. Must not be `NULL`.
 */
static void build_in_tiny_arena(void* userdata) {
  static _Alignas(max_align_t) char memory[TINY_ARENA_SIZE];
  struct Arena rest = {.beg = memory, .end = memory + sizeof(memory)};
  const struct Arena perm = arena_split(&rest, sizeof(memory) / 4);
  const struct Arena scratch = arena_split(&rest, sizeof(memory) / 4);
  struct JobArenas worker_arenas[1] = {
      {.perm = arena_split(&rest, sizeof(memory) / 4), .scratch = rest}};
  struct Log log = test_log();
  const struct BuildOptions options = {.worker_count = 1};
  (void)cmd_build_execute(&options, worker_arenas, userdata, perm, scratch, &log);
}

// A configured album template name is used instead of the default.
static void test_honors_configured_album_template(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, "album_template = \"album-page.html\"\n");
  write_jpeg(os, "photos/one.jpg");
  os_memory_write(os, "templates/album-page.html", "<main>{{album.title}}</main>\n");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));

  TEST_CHECK(os_memory_is_file(os, "public/index.html", "<main>Test Gallery</main>\n"));
}

// A build writes exactly the manifest's intended outputs: album pages, derivatives, originals,
// aggregate outputs and static files, and nothing for a file in `input_dir` that is not media.
static void test_writes_exactly_manifest_outputs(void) {
  struct Os* os = os_memory();
  static const char* const expected[] = {
      "public/_fram/originals/album/one-jpg.jpg",
      "public/_fram/v1-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.jpg",
      "public/_fram/v1-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.jpg",
      "public/album/index.html",
      "public/fram.css",
      "public/index.html",
      "public/sitemap.xml",
  };
  struct Log log = test_log();
  write_gallery_fixture(os, "aggregate_templates = [\"sitemap.xml\"]\n");
  os_memory_write(os, "templates/sitemap.xml", "sitemap\n");
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "photos/album/notes.txt", "not media\n");
  os_memory_write(os, "static/fram.css", "body{}\n");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // Exactly the expected set, so neither an extra output nor the non-media file passes.
  TEST_CHECK(is_file_list(os, "public", expected, sizeof(expected) / sizeof(expected[0])));
}

// A trailing slash on `input_dir` does not leak the directory name into the output tree. The prefix
// strip that places each media item in its album compares `<input_dir>/`, so an unnormalized
// `"photos/"` would miss and publish `public/photos/album/index.html`.
static void test_tolerates_trailing_slash_on_input_dir(void) {
  struct Os* os = os_memory();
  // The shared config sets `input_dir`, so this fixture writes its own.
  os_memory_write(os, "fram.toml",
                  "title = \"Test Gallery\"\n"
                  "author = \"Tests\"\n"
                  "input_dir = \"photos/\"\n"
                  "[derivatives.s]\nwidth = 8\nheight = 8\nquality = 70\ncrop = true\n"
                  "[derivatives.m]\nwidth = 16\nheight = 16\nquality = 75\n");
  os_memory_write(os, "templates/album.html", ALBUM_TEMPLATE);
  write_jpeg(os, "photos/album/one.jpg");
  struct Log log = test_log();

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // No output, page, derivative or original, carries the name of `input_dir`.
  static const char* const expected[] = {
      "public/_fram/originals/album/one-jpg.jpg",
      "public/_fram/v1-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.jpg",
      "public/_fram/v1-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.jpg",
      "public/album/index.html",
      "public/index.html",
  };
  TEST_CHECK(is_file_list(os, "public", expected, sizeof(expected) / sizeof(expected[0])));
}

// A gallery that ships no `static` directory builds. `static_dir` is optional with a default, so an
// absent one is skipped rather than reported.
static void test_builds_with_no_static_dir(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  TEST_CHECK(!os_memory_has_path(os, "static"));

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_has_path(os, "public/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/album/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/album/one-jpg.jpg"));
}

// A `static` directory is mirrored into `output_dir` as-is, so a top-level file lands at the output
// root and a nested one keeps its directories.
static void test_copies_static_files_when_present(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "static/robots.txt", "User-agent: *\n");
  os_memory_write(os, "static/assets/fram.css", "body{}\n");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_has_path(os, "public/robots.txt"));
  TEST_CHECK(os_memory_has_path(os, "public/assets/fram.css"));
}

// Same-named sources in different directories publish to distinct, non-colliding outputs. One file
// name repeated in three nested albums is not a collision, because the album slug is part of every
// output path. Each directory level still gets its own album page.
static void test_distinguishes_same_name_in_different_dirs(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/one.jpg");
  write_jpeg(os, "photos/trip/one.jpg");
  write_jpeg(os, "photos/trip/deep/one.jpg");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_has_path(os, "public/trip/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/trip/deep/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/one-jpg.jpg"));
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/trip/one-jpg.jpg"));
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/trip/deep/one-jpg.jpg"));
}

// An album holding only sub-directories has no media of its own. Its own page is still written, and
// the zero-length media and sub-album arrays it allocates are valid rather than a failure.
static void test_builds_an_album_with_no_media_of_its_own(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/container/inner/one.jpg");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_has_path(os, "public/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/container/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/container/inner/index.html"));
}

// A second build over a finished output tree succeeds and produces the same set of outputs.
static void test_rebuild_succeeds_and_repeats_its_outputs(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");

  TEST_CHECK(execute_build(os, &log) == 0);
  const ptrdiff_t public_count_built = os_memory_files(os, "public").count;
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // The album page, the root page, both derivatives and the copied original: the second build adds
  // nothing and removes nothing.
  TEST_CHECK(public_count_built == 5);
  TEST_CHECK(os_memory_files(os, "public").count == public_count_built);
}

// An `input_dir` inside `output_dir` builds, because no output lands inside it: `output_dir = "."`
// with `input_dir = "photos"` puts the gallery beside its sources. A rebuild succeeds and repeats
// the same outputs, so neither walk reads the first build's derivatives back as media.
static void test_builds_and_rebuilds_with_output_dir_holding_input_dir(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, "output_dir = \".\"\n");
  write_jpeg(os, "photos/album/one.jpg");

  ptrdiff_t output_counts[2] = {0, 0};
  for (int i = 0; i < 2; i++) {
    struct Log log = test_log();
    TEST_CHECK(execute_build(os, &log) == 0);
    TEST_CHECK(test_log_is_empty(&log));
    output_counts[i] = os_memory_files(os, ".").count;
  }
  // The config, the template, and the source, then the album page, the root page, both derivatives
  // and the copied original.
  TEST_CHECK(output_counts[0] == 8);
  TEST_CHECK(output_counts[1] == output_counts[0]);
  TEST_CHECK(os_memory_has_path(os, "index.html"));
  TEST_CHECK(os_memory_has_path(os, "album/index.html"));
  TEST_CHECK(os_memory_has_path(os, "_fram/originals/album/one-jpg.jpg"));
}

// An `output_dir` that a symlink inside `input_dir` reaches is left out of the media walk, so a
// rebuild does not read the first build's outputs back as sources. The derivatives here are JPEG
// files, which the walk would otherwise discover as media.
static void test_rebuild_skips_output_dir_linked_from_input_dir(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_symlink(os, "../public", "photos/published");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // The album page, the root page, both derivatives and the copied original, and no album for the
  // linked output tree.
  TEST_CHECK(os_memory_files(os, "public").count == 5);
  TEST_CHECK(!os_memory_has_path(os, "public/published"));
}

// A caller-supplied `worker_count` reaches every pool instead of being replaced by the detected
// default. A build with it produces the same bytes as the default build. The resolved count is not
// observable from the return value, so it is read off the verbose phase lines, which are the only
// place the module reports it. Workers finish their jobs in any order, so the progress lines are
// not compared.
static void test_honors_requested_worker_count(void) {
  // One worker, requested explicitly, must be honored rather than read as unset. A guard spelled
  // `worker_count != 1` instead of `!= 0` swallows that request and falls back to the detected core
  // count, which asking for `2` cannot see. Asking for one is also the only way to ask for a serial
  // build. A request the build dropped would show the detected count. Both cases assume the host
  // reports at least three cores.
  static const ptrdiff_t worker_counts[] = {2, 1};
  static const char* const photo_paths[] = {"photos/a.jpg", "photos/b.jpg", "photos/c.jpg"};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(worker_counts) / sizeof(worker_counts[0])); i++) {
    struct Os* os = os_memory();
    write_gallery_fixture(os, NULL);
    for (ptrdiff_t j = 0; j < (ptrdiff_t)(sizeof(photo_paths) / sizeof(photo_paths[0])); j++) {
      write_jpeg(os, photo_paths[j]);
    }
    const struct BuildOptions options = {.worker_count = worker_counts[i], .is_verbose = true};
    TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
    TEST_CHECK(os_memory_is_printed(os, 1, ""));
    struct Arena arena = test_arena();
    struct Buf probe_line = {.arena = &arena};
    struct Buf derivative_line = {.arena = &arena};
    struct Buf album_line = {.arena = &arena};
    buf_printf(&probe_line, "probing media, workers: %td", worker_counts[i]);
    buf_printf(&derivative_line, "generating derivatives, workers: %td", worker_counts[i]);
    buf_printf(&album_line, "rendering albums, workers: %td", worker_counts[i]);
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&probe_line)));
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&derivative_line)));
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&album_line)));
    // The requested count changes only how the work is scheduled, never the output.
    TEST_CHECK(os_memory_is_file(os, "public/index.html",
                                 "<!doctype html><title>Test Gallery - Test Gallery</title><ul>"
                                 "<li>a.jpg</li><li>b.jpg</li><li>c.jpg</li></ul>"));
  }
}

// A build runs at most four workers per online CPU, however many it is asked for, and the verbose
// phase lines name that count. `BUILD_WORKER_PER_CPU_MAX` is file-local to `cmd_build.c`, so the
// four is spelled here, and changing it must change this test.
static void test_caps_the_worker_count_at_four_per_cpu(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/one.jpg");
  const struct BuildOptions options = {.worker_count = 100000, .is_verbose = true};
  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
  static const char* const phases[] = {"probing media", "generating derivatives",
                                       "rendering albums"};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(phases) / sizeof(phases[0])); i++) {
    struct Arena arena = test_arena();
    struct Buf line = {.arena = &arena};
    buf_printf(&line, "%s, workers: %td", phases[i], 4 * pool_resolve_worker_count());
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&line)));
  }
  TEST_CHECK(os_memory_has_path(os, "public/index.html"));
}

// Under an address-space limit the build halves its worker count, down to one, before it halves any
// arena, and the verbose phase lines name the count it kept. The limits follow the sizes in
// `cmd_build.c`: 30 GiB lies between the 22 GiB two workers need and the 36.5 GiB four need, and
// 10 GiB is under the 14.75 GiB of one, which then runs on arenas halved twice.
static void test_cuts_workers_before_arenas_under_limit(void) {
  static const struct {
    ptrdiff_t len_max;
    ptrdiff_t worker_count;
  } cases[] = {
      {(ptrdiff_t)30 << 30, 2},
      {(ptrdiff_t)10 << 30, 1},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Os* os = os_memory();
    write_gallery_fixture(os, NULL);
    write_jpeg(os, "photos/one.jpg");
    const struct BuildOptions options = {.worker_count = 4, .is_verbose = true};
    arena_reserve_len_max = cases[i].len_max;
    TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
    arena_reserve_len_max = PTRDIFF_MAX;
    struct Arena arena = test_arena();
    struct Buf line = {.arena = &arena};
    buf_printf(&line, "probing media, workers: %td", cases[i].worker_count);
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&line)));
    TEST_CHECK(os_memory_has_path(os, "public/index.html"));
    TEST_MSG("case %td", i);
  }
}

// An album of many items builds. A page render's temporaries grow with the items it lists, about a
// kilobyte each.
static void test_builds_a_large_album(void) {
  enum { ITEM_COUNT = 500 };
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  os_memory_write(os, "templates/album.html", LARGE_ALBUM_TEMPLATE);
  struct Arena arena = test_arena();
  struct Str jpeg = {0};
  if (!TEST_CHECK(test_jpeg_encode(2, 2, &arena, &jpeg) == 0)) {
    return;
  }
  for (int i = 0; i < ITEM_COUNT; i++) {
    char path[64];
    (void)snprintf(path, sizeof(path), "photos/big/%05d.jpg", i);
    os_memory_write_bytes(os, str_from_cstr(path), jpeg);
  }
  const struct BuildOptions options = {.worker_count = 1};
  struct Log log = test_log();
  TEST_CHECK(execute_build_with_options(&options, os, &log) == 0);
  TEST_MSG("errors: %s", buf_cstr(&log.buf));
}

// A gallery with no media skips the probe and derivative phases, so the verbose log carries no
// status or progress line for either. A phase with no jobs returns before it prints, so an empty
// gallery does not report a worker count for work it never starts. The album page phase still runs,
// for the root album alone.
static void test_skips_empty_phases_in_verbose_output(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  // A file that is not media keeps `input_dir` present while the gallery holds no media.
  os_memory_write(os, "photos/notes.txt", "not media\n");
  const struct BuildOptions options = {.worker_count = 3, .is_verbose = true};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "discovering media\n"
                                  "discovering static files\n"
                                  "planning albums\n"
                                  "building output manifest\n"
                                  "rendering albums, workers: 3\n"
                                  "\rrendering albums 1/1\n"
                                  "writing album pages\n"
                                  "rendering aggregate templates\n"
                                  "copying static files\n"
                                  "build complete\n"));
}

// A build that succeeds leaves its log empty, which is the other half of `cmd_build_execute`'s
// `log` contract. Without this a phase that logged on the success path would go unnoticed, since
// every other test that inspects the log is a failure test.
static void test_leaves_log_empty_on_success(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// `cmd_build_run` reports failure as `EXIT_CODE_FAILURE` and prints the collected diagnostic to
// standard error exactly once, which no other test asserts. Every other test routes failures
// through `cmd_build_execute` to inspect the message, so without this the boundary itself is
// unasserted: the exit code and the single print.
static void test_run_prints_diagnostic_once_and_fails(void) {
  // No `fram.toml`, the earliest failure, so the diagnostic is a single known line.
  struct Os* os = os_memory();
  const struct BuildOptions options = {0};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  // Compared whole rather than by substring: "exactly once" is the claim, and only a whole-buffer
  // comparison can tell one print from two.
  struct Arena arena = test_arena();
  struct Buf expected = {.arena = &arena};
  buf_printf(&expected, "failed to read config: %s ('fram.toml')\n", strerror(ENOENT));
  TEST_CHECK(os_memory_is_printed(os, 2, buf_cstr(&expected)));
}

// A build whose arena is too small for its data ends in `os_oom` rather than failing with a
// diagnostic or overrunning the arena. Under the test build's `os_oom` that is the process exiting
// with `TEST_OOM_EXIT_STATUS`. `main.c`'s `os_oom` exits with `EXIT_CODE_FAILURE` instead, so this
// test does not pin the production exit status.
static void test_tiny_arena_ends_in_oom(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  TEST_CHECK(test_is_oom(build_in_tiny_arena, os));
}

// A missing `fram.toml` is logged with its cause, and nothing prints it before the command
// boundary. The cause is part of the claim: a missing config must not read like an unreadable one.
static void test_reports_missing_config(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log, "failed to read config: %s ('fram.toml')\n", strerror(ENOENT)));
}

// `input_dir` is required, so an absent one is a failure, reported as a resolution failure carrying
// the system cause. `ENOENT` and `EACCES` need different fixes and nothing later would report
// either, so the cause has to appear here. Only the optional read root is skipped when missing.
// Compared exactly, so a caller that appended the path a second time would fail.
static void test_reports_absent_input_dir(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to resolve config directory 'input_dir': %s ('photos')\n",
                         strerror(ENOENT)));
}

// `templates_dir` is required the same way, and reports the key the user has to fix rather than the
// one that happened to be checked first. A root that cannot be inspected reports its own cause.
static void test_reports_absent_templates_dir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "fram.toml", GALLERY_CONFIG);
  write_jpeg(os, "photos/album/one.jpg");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'templates_dir': %s ('templates')\n",
                         strerror(ENOENT)));

  os_memory_fail(os, OS_MEMORY_OP_STAT, "photos", EACCES);
  log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'input_dir': %s ('photos')\nfailed "
                         "to resolve config directory 'templates_dir': %s ('templates')\n",
                         strerror(EACCES), strerror(ENOENT)));
}

// A directory key the config load rejects is reported once. Its default names a directory the user
// never chose, so the build does not go on to report that default missing as well.
static void test_reports_a_rejected_directory_key_once(void) {
  static const char* const cases[][4] = {
      {"input_dir = 5\n", "photos/album/one.jpg", "templates/album.html",
       "config key 'input_dir' must be a string"},
      {"input_dir = \"/\"\n", "photos/album/one.jpg", "templates/album.html",
       "config key 'input_dir' must not be empty"},
      {"input_dir = \"photos\"\ntemplates_dir = [\"layouts\"]\n", "photos/album/one.jpg",
       "layouts/album.html", "config key 'templates_dir' must be a string"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i][0]);
    struct Os* os = os_memory();
    struct Arena config_arena = test_arena();
    struct Buf config = {.arena = &config_arena};
    buf_append(&config, S("title = \"Test Gallery\"\nauthor = \"Tests\"\n"));
    buf_append(&config, str_from_cstr(cases[i][0]));
    buf_append(&config, S("[derivatives.s]\nwidth = 8\nheight = 8\nquality = 70\n"));
    os_memory_write(os, "fram.toml", buf_cstr(&config));
    write_jpeg(os, cases[i][1]);
    os_memory_write(os, cases[i][2], ALBUM_TEMPLATE);
    struct Log log = test_log();
    TEST_CHECK(execute_build(os, &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][3]));
  }
  TEST_CASE_(NULL);
}

// A `templates_dir` that exists but is a regular file is reported as the wrong type, which is its
// own cause rather than an `errno`, so the message carries no system text.
static void test_reports_templates_dir_that_is_a_file(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  os_memory_write(os, "fram.toml", GALLERY_CONFIG);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "templates", "not a directory\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'templates_dir': not a directory "
                         "('templates')\n"));
}

// A configured `output_dir` that already exists as a regular file is reported once the manifest has
// passed, where the build creates `output_dir`, before any derivative is written. The reason names
// the component that failed and the caller does not repeat the configured root, so the message is
// compared whole.
static void test_reports_unusable_output_dir(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  // `public` is the default `output_dir`, so a file there is what makes `os_mkdirs` fail.
  os_memory_write(os, "public", "not a directory");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to prepare output directory: exists and is not a directory ('public')\n"));
}

// An `output_dir` that is a dangling symlink is reported once the manifest has passed, where the
// build creates `output_dir`, before any derivative is written. A dangling symlink has no identity,
// so the manifest sees nothing there, and `os_mkdirs` then fails the inspection that follows
// `mkdir`'s `EEXIST`. The message is compared whole.
static void test_reports_dangling_output_dir_symlink(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  // `public` is the default `output_dir`, so a dangling symlink there is what makes `os_mkdirs`
  // fail.
  os_memory_symlink(os, "missing", "public");
  struct Log log = test_log();

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to prepare output directory: cannot inspect directory: %s ('public')\n",
      strerror(ENOENT)));
}

// An `output_dir` below `input_dir` is refused before media is discovered or probed, so no earlier
// build's output can be read back as a source, and it is refused although it does not exist yet.
// The verbose phase lines show that the build stopped before the media walk, and the refused build
// creates none of the nested directories.
static void test_rejects_output_dir_inside_input_dir(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, "output_dir = \"photos/generated/site\"\n");
  write_jpeg(os, "photos/album/one.jpg");
  const struct BuildOptions options = {.is_verbose = true};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "output directory would write inside 'input_dir': "
                                  "'photos/generated/site'\n"));
  TEST_CHECK(!os_memory_has_path(os, "photos/generated"));
}

// Slugging is lossy, so two different source names can claim one output path. The manifest reports
// the collision and names both sources instead of letting the second write win, once, at the first
// output the two share, although they share both derivatives and the original. A non-ASCII byte
// folds to hex, so `caf\xC3\xA9` produces the slug a literal `cafc3a9` also produces. The refused
// build creates no `output_dir`, because nothing is written before the manifest passes.
static void test_rejects_colliding_source_names(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/caf\xC3\xA9.jpg");
  write_jpeg(os, "photos/album/cafc3a9.jpg");

  TEST_CHECK(execute_build(os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(
      test_log_is(&log,
                  "duplicate output path for 'photos/album/cafc3a9.jpg' and "
                  "'photos/album/caf\xC3\xA9.jpg': "
                  "'public/_fram/v1-s-8x8c-q70-m-16x16-q75-f1000/album/cafc3a9-jpg-s.jpg'\n"));
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A static file copied to the output root shares its paths with the generated pages, so one named
// after the root album page is rejected as a duplicate of it rather than overwriting it or being
// overwritten. The refused build creates no `output_dir`.
static void test_rejects_static_file_colliding_with_album_page(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "static/index.html", "static\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'photos' and "
                         "'static/index.html': 'public/index.html'\n"));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// An album template that fails to parse fails the build at the page phase. The diagnostic carries
// the parser's cause, its position and the template name, followed by the album that was being
// rendered, so a template error reads apart from a template that cannot be read. This and
// `test_reports_a_shared_render_failure_once` are the tests that reach the build's page-render arm.
static void test_reports_bad_template(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, "album_template = \"broken.html\"\n");
  // The section never closes, so the template is read but fails to compile.
  os_memory_write(os, "templates/broken.html", "{{#media}}unclosed");
  write_jpeg(os, "photos/one.jpg");

  TEST_CHECK(execute_build(os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log,
                         "section-opening tag has no closer at line 1, column 1 (in 'broken.html') "
                         "(while rendering the root album)\n"));
}

// Two albums that fail to render through one missing template fail for one cause. The report prints
// the first album's line whole and counts the second, and both count as errors.
static void test_reports_a_shared_render_failure_once(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, "album_template = \"missing.html\"\n");
  write_jpeg(os, "photos/album/one.jpg");

  TEST_CHECK(execute_build(os, &log) == -1);
  // Albums are sorted with the root first, so the root album is the one printed.
  TEST_CHECK(test_log_is(&log,
                         "failed to read template: %s ('templates/missing.html') (while rendering "
                         "the root album)\n\xE2\x80\xA6 and 1 more failures\n",
                         strerror(ENOENT)));
  TEST_CHECK(log.errors == 2);
}

// Two media items that both fail to probe log one line each, so neither is lost to the other. The
// lines differ in the source they name, so the report collapses neither, and a log that overwrote,
// or that ran two messages together, would still satisfy every single-item test.
static void test_reports_one_line_per_failing_media_item(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  // Not JPEG data, so the probe fails for both sources.
  os_memory_write(os, "photos/album/a.jpg", "not a JPEG\n");
  os_memory_write(os, "photos/album/b.jpg", "not a JPEG\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  // Sources are walked in sorted order, so `a.jpg` precedes `b.jpg`. Compared whole, with the
  // separator in the middle, so a dropped newline or a lost line both fail here.
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: failed to probe JPEG: unknown image type "
                         "('photos/album/a.jpg')\n"
                         "failed to probe media: failed to probe JPEG: unknown image type "
                         "('photos/album/b.jpg')\n"));
  TEST_CHECK(log.errors == 2);
}

// Several independent mistakes in one gallery are all reported by one build, and the build writes
// nothing: an unknown config key, two sources that cannot be probed, two sources whose names slug
// alike, and a static file that claims the root album's page. Each kind comes from a different
// phase, so the order is the phase order, and within a phase it is the sorted walk order. The probe
// failures do not stop the build before the manifest, because no output path depends on a probed
// dimension.
static void test_reports_every_independent_error(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, "colour = \"blue\"\n");
  os_memory_write(os, "photos/album/bad-a.jpg", "not a JPEG\n");
  os_memory_write(os, "photos/album/bad-b.jpg", "not a JPEG\n");
  write_jpeg(os, "photos/album/caf\xC3\xA9.jpg");
  write_jpeg(os, "photos/album/cafc3a9.jpg");
  os_memory_write(os, "static/index.html", "static\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "unknown config key 'colour'\n"
                         "failed to probe media: failed to probe JPEG: unknown image type "
                         "('photos/album/bad-a.jpg')\n"
                         "failed to probe media: failed to probe JPEG: unknown image type "
                         "('photos/album/bad-b.jpg')\n"
                         "duplicate output path for 'photos/album/cafc3a9.jpg' and "
                         "'photos/album/caf\xC3\xA9.jpg': "
                         "'public/_fram/v1-s-8x8c-q70-m-16x16-q75-f1000/album/cafc3a9-jpg-s.jpg'\n"
                         "duplicate output path for 'photos' and 'static/index.html': "
                         "'public/index.html'\n"));
  TEST_CHECK(log.errors == 5);
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A JPEG over the decoder's input limit is rejected by its probe read. The file is sparse, so the
// fixture costs no memory.
static void test_rejects_oversize_source_at_probe(void) {
  const ptrdiff_t big_source_len = (ptrdiff_t)4 * 1024 * 1024 * 1024;
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/big.jpg");
  os_memory_truncate(os, "photos/big.jpg", big_source_len);
  const struct BuildOptions options = {.worker_count = 1};
  struct Log log = test_log();

  TEST_CHECK(execute_build_with_options(&options, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: exceeds max file size (%d bytes) at %td "
                         "bytes ('photos/big.jpg')\n",
                         IMAGE_INPUT_LEN_MAX, big_source_len));
}

// An output path that cannot be opened as a file fails the build at the write phase, after every
// album has rendered successfully. That is the last of `cmd_build_execute`'s phase arms and the
// only one reached with a complete set of rendered pages in hand, so a write failure reported as
// success would otherwise pass the suite with the pages silently missing. One page path is an
// existing directory. The phase stops at its first failure, so a second build fails the root
// album's page, which is written first, with an injected `EACCES`, as an unwritable directory would
// refuse it.
static void test_reports_unwritable_output(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  // A directory at the page path cannot be opened as a file.
  os_memory_mkdir(os, "public/album/index.html");
  struct Log log = test_log();

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to write output for 'album': %s ('public/album/index.html')\n",
                         strerror(EISDIR)));

  os_memory_fail(os, OS_MEMORY_OP_WRITE, "public/index.html", EACCES);
  log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to write output for the root album: %s ('public/index.html')\n",
                         strerror(EACCES)));
}

// A `static_dir` that exists but is a regular file is reported. Skipping an absent optional root
// must not also skip a misconfigured one, which is why the skip asks whether anything is there
// rather than whether a directory is there: the wrong type survives the skip and reaches the type
// check, which names it.
static void test_reports_static_dir_that_is_a_file(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "static", "not a directory\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'static_dir': not a directory "
                         "('static')\n"));
}

TEST_LIST = {
    {"honors configured album template", test_honors_configured_album_template},
    {"writes exactly manifest outputs", test_writes_exactly_manifest_outputs},
    {"tolerates trailing slash on input_dir", test_tolerates_trailing_slash_on_input_dir},
    {"builds with no static dir", test_builds_with_no_static_dir},
    {"copies static files when present", test_copies_static_files_when_present},
    {"distinguishes same name in different dirs", test_distinguishes_same_name_in_different_dirs},
    {"builds an album with no media of its own", test_builds_an_album_with_no_media_of_its_own},
    {"rebuild succeeds and repeats its outputs", test_rebuild_succeeds_and_repeats_its_outputs},
    {"builds and rebuilds with output dir holding input dir",
     test_builds_and_rebuilds_with_output_dir_holding_input_dir},
    {"rebuild skips output dir linked from input dir",
     test_rebuild_skips_output_dir_linked_from_input_dir},
    {"honors requested worker count", test_honors_requested_worker_count},
    {"caps the worker count at four per CPU", test_caps_the_worker_count_at_four_per_cpu},
    {"cuts workers before arenas under limit", test_cuts_workers_before_arenas_under_limit},
    {"builds a large album", test_builds_a_large_album},
    {"skips empty phases in verbose output", test_skips_empty_phases_in_verbose_output},
    {"leaves log empty on success", test_leaves_log_empty_on_success},
    {"run prints diagnostic once and fails", test_run_prints_diagnostic_once_and_fails},
    {"tiny arena ends in oom", test_tiny_arena_ends_in_oom},
    {"reports missing config", test_reports_missing_config},
    {"reports absent input dir", test_reports_absent_input_dir},
    {"reports absent templates dir", test_reports_absent_templates_dir},
    {"reports a rejected directory key once", test_reports_a_rejected_directory_key_once},
    {"reports templates dir that is a file", test_reports_templates_dir_that_is_a_file},
    {"reports unusable output dir", test_reports_unusable_output_dir},
    {"reports dangling output dir symlink", test_reports_dangling_output_dir_symlink},
    {"rejects output dir inside input dir", test_rejects_output_dir_inside_input_dir},
    {"rejects colliding source names", test_rejects_colliding_source_names},
    {"rejects static file colliding with album page",
     test_rejects_static_file_colliding_with_album_page},
    {"reports bad template", test_reports_bad_template},
    {"reports a shared render failure once", test_reports_a_shared_render_failure_once},
    {"reports one line per failing media item", test_reports_one_line_per_failing_media_item},
    {"reports every independent error", test_reports_every_independent_error},
    {"rejects oversize source at probe", test_rejects_oversize_source_at_probe},
    {"reports unwritable output", test_reports_unwritable_output},
    {"reports static dir that is a file", test_reports_static_dir_that_is_a_file},
    {NULL, NULL},
};
