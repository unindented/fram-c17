# Architecture

`fram` is a small C17 static photo and video gallery generator. It reads project configuration, a directory tree of media, and Mustache templates. It writes the generated files to a gallery-owned directory. This document describes the code structure. See [README.md](README.md) for usage and template behavior.

## Layout

- [src/app/](src/app/): Process entry point, command-line interface, and command boundaries.
- [src/build/](src/build/): Build orchestration, parallel phases, templates, manifest population, and output writing.
- [src/domain/](src/domain/): Gallery configuration, albums, media items, and the output manifest.
- [src/formats/](src/formats/): Boundaries for HTML, JPEG, EXIF, and TOML data.
- [src/runtime/](src/runtime/): The host interface and its POSIX implementation, the directory walk, process and video services on the real host, the out-of-memory report, and the thread pool.
- [src/core/](src/core/): Small reusable primitives, containers, diagnostics, text, and path logic.
- [tests/](tests/): Golden test suite, the shared unit-test support library ([tests/test_support.h](tests/test_support.h)), and the in-memory host ([tests/os_memory.h](tests/os_memory.h)).
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): Project entry point and supported build configurations.
- [cmake/](cmake/): Build profiles, quality tools, packaging, the golden test driver, and reusable cross toolchains. [BUILD.md](BUILD.md) documents their boundaries and policy.
- [vendor/](vendor/): Bundled dependencies (`sharedstuff`, `copt`, `tomlc17`, `libjpeg-turbo`, `libwebp`, `stb`, `mustache4c`, `acutest`).

First-party dependencies point inward:

- `app` can depend on all inner directories.
- `build` can depend on `domain`, `formats`, `runtime`, and `core`.
- `domain` can depend on `formats`, `runtime`, and `core`.
- `formats` and `runtime` can depend on `core`.
- `core` cannot depend on another first-party directory.

A module can skip layers. Modules in the same directory can depend on each other. First-party include directives start at the `src/` root. For example, a module includes `"core/ascii.h"`. This convention makes each dependency visible at the call site. Vendored support is outside the first-party hierarchy.

## Entry point and commands

`main` ([src/app/main.c](src/app/main.c)) only calls `cli_dispatch` ([src/app/cli_dispatch.h](src/app/cli_dispatch.h)) on the POSIX host. This separation lets unit tests link the code that maps an action to an exit code. `cli_dispatch` uses `cli_parse` ([src/app/cli.h](src/app/cli.h)) to parse the command line. `cli_parse` selects a subcommand and one of four actions: version, help, run, or error. For the run action, `cli_dispatch` calls one of these functions:

- `cmd_build_run` ([src/app/cmd_build.h](src/app/cmd_build.h)): generates the gallery.
- `cmd_config_run` ([src/app/cmd_config.h](src/app/cmd_config.h)): loads `fram.toml` and prints the resolved config.

`cli.c` is the one translation unit that defines `COPT_IMPL`. `cli_parse` never prints or exits. `cli_dispatch` prints a rejected command line's diagnostic, `--help` and `--version` through `os_print`, and formats each in a reservation of its own, which it releases once the command or the print returns. A command reserves its own memory. Every command returns `enum ExitCode` ([src/app/exit_code.h](src/app/exit_code.h)): `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).

## Build pipeline

The build has two layers:

- `cmd_build_run` is the outer boundary. It resolves the worker count, reserves the build's memory once, as one block of address space, and splits the block into arenas: the build's log, and a perm, for data that outlives a phase, and a scratch, for temporaries, for the main thread and for each worker. It calls `cmd_build_execute`, and releases the block. On failure, it prints the build's log to `stderr` exactly once, through `os_print`.
- `cmd_build_execute` owns the `BuildState`, runs the eight phases below in sequence over the arenas it is given, and writes the verbose log. It runs the first phase itself, and `run_build_phases` runs the other seven. [album_scanner](src/build/album_scanner.h) builds the album tree and probes media. [manifest_builder](src/build/manifest_builder.h) plans output paths. [derivative_renderer](src/build/derivative_renderer.h) generates media output. [page_renderer](src/build/page_renderer.h) renders album pages. [site_writer](src/build/site_writer.h) writes the generated gallery.

No phase writes a failure diagnostic directly to `stderr`, apart from the out-of-memory report, which `os_oom` prints through `memory_oom_report` on the way out of the process. Every fallible function records its errors in a `struct Log`, a growable buffer with one line per error and an error count, which the command boundary prints once. Until the first write the build keeps going past an error wherever the next phase still has something to check, so one run reports every bad config key and every album and source in the album skeleton that is past a path limit, apart from the sub-albums and sources of an album already reported, which fail with it, and, when the skeleton is whole, every source that cannot be probed and every output collision, and then stops before creating `output_dir` if any error was recorded. A config that cannot be read, a rejected directory key, a root that cannot be read or walked, a missing video tool, and a skeleton error stop the build before the next phase, because they leave it nothing to check. After the first write a failed phase is the last one that runs: the derivative phase still generates every other media item's outputs and reports each item that fails, the page phase still renders every other album and reports each one that fails, and `site_writer` stops at the first album page, aggregate template or static file that fails. A parallel phase gives each job a log of its own and appends the failed jobs' logs in job order, at most twenty distinct ones, plus a count of the remainder. An album page render ends its failure with `(while rendering album '<dir>')` or `(while rendering the root album)`, and the comparison stops there, so one template failure that every page repeats prints once.

Verbose status is separate from diagnostics, and both go through `os_print`. The main thread writes phase status lines between phases. Each worker formats its `\r<label> n/total` progress line whole, such as `probing media 3/12`, and prints it in one `os_print` call under a lock that `job.c` keeps for it, so two workers' lines never interleave.

The phase order follows the data dependencies. A template can read media dimensions, album covers, and descendant counts. These values are not complete until the tool builds the album tree and probes every media item. The build therefore constructs all arena-owned strings serially, then uses separate parallel phases to probe media, generate derivatives, and render pages.

1. **Load inputs** (`load_build_inputs`): The tool uses `gallery_config_load` to read and validate `fram.toml`. It requires each configured read root to be a usable directory, reporting the cause when one is not, and skipping the optional static root when a gallery ships none. `manifest_builder_check_output_dir` then rejects an `output_dir` at or below `input_dir`, `templates_dir`, or `static_dir`, before any input tree is walked. Then `walk_files_with_suffixes(paths, input_dir, output_dir, suffixes, ...)` finds and sorts the JPEG and MP4 sources in one walk and the static files in another. Every walk of an input tree leaves `output_dir` out by identity, so a symlink into the output tree cannot feed an earlier build's outputs back in as inputs. When an MP4 exists, the phase verifies `ffmpeg` and `ffprobe` once. The block was reserved at startup, with arenas for at most four workers per online CPU, however many `-w` asks for, and a parallel phase starts no more workers than it has jobs, so `-w` is a maximum.
2. **Build album skeleton** (`build_album_skeleton` -> `album_scanner_build_skeleton`): The tool walks the sorted media paths serially. It creates the root and nested albums, allocates every string and media record in the main thread's perm, slugifies generated album and media output names, plans page and media paths, and builds the flat media array. It then fills parent, child, cover, and count relationships, reversing each album's child and media arrays when the configured sort order is descending, before it picks covers. The flat media array and the album array stay in source order. Raw directory basenames become album titles with `_` displayed as a space. The phase also allocates one video-frame slot per media item, with its temporary path for a video, and one rendered-page slot per album.
3. **Probe media** (`probe_media` -> `album_scanner_probe_media`): `job_run` dispatches one job for each media item. An image job reads the source, probes its JPEG dimensions, reads its EXIF orientation, and calculates its presented and derivative dimensions. A video job probes duration, chooses the configured frame time or the midpoint when that time is beyond the end, extracts one auto-rotated temporary JPEG into its frame slot, and probes that frame's dimensions. Each job reads into a copy of its worker's scratch arena, up to `IMAGE_INPUT_LEN_MAX`, so a source or frame over the limit fails that job alone, and gives the pages of a source of 16 MiB or more back with `arena_discard` once it is probed. Each job writes only scalar fields in its media record, its own frame slot, and its errors to its own log, in its worker's log share. After all workers finish, `pool_run` joins them. Then `job_run` appends each distinct job's log to the build's log, in job order, up to twenty, followed by a count of the remaining failures. The optional progress line is the only worker output to `stderr`, apart from an out-of-memory report. The build removes every frame a slot still marks as created when it ends, on success and on failure. An out-of-memory exit skips that cleanup and leaves the current frames in `TMPDIR`, and a running `ffmpeg` child finishes its frame after the exit, as Ctrl-C and `kill` leave them, because `src/` installs no signal handlers.
4. **Manifest** (`populate_output_manifest` -> `manifest_builder_populate`): The tool records all output paths for album pages, derivatives, originals, aggregates, and static files. It does this before it writes a generated file. It rejects four types of conflict:
   1. Two producers can claim the same path. `manifest_add` rejects the second claim. Paths that differ only in ASCII case are the same path, because a case-insensitive filesystem such as the macOS default stores them as one file. The comparison folds only `A-Z` to `a-z`. Other bytes must match exactly.
   2. One output can use `a/b` while another uses `a/b/c`. The first path must be a file, but the second path requires a directory at `a/b`. `manifest_find_prefix_collision` detects this conflict after the tool records all paths. It folds ASCII case in the same way, so `A/B` and `a/b/c` also conflict.
   3. An output can name an existing build input. `manifest_builder_populate` compares the device and inode of each file. It checks the config, all discovered media and static files, and every file below `templates_dir`. The template tree includes partials and templates that no configuration names. A missing `templates_dir` adds no inputs, although load inputs has already rejected one in a full build. A `templates_dir` that exists but cannot be walked fails the build.
   4. An output can land at or below `input_dir`, `templates_dir`, or `static_dir`, even where no file exists yet. The next build would discover a new file below `input_dir` as media and would copy a new file below `static_dir` as a static file. The same build could read a new file below `templates_dir` as a partial. `manifest_builder_check_output_dir` has already compared the device and inode of each root with the nearest existing ancestor of `output_dir` and each of that directory's ancestors, during load inputs. That rejects an `output_dir` inside a root before any walk, whether or not `output_dir` exists yet. `manifest_builder_populate` then compares each root with each existing directory between `output_dir` and the output. That catches a symlink inside `output_dir` that leads into a root. These comparisons detect alternate spellings and symlinked roots. A sibling directory such as `photosx` does not match. A missing root is skipped. The tool runs this check before the input-file check, so an output below a root reports this conflict.
5. **Prepare output** (`prepare_output_dir`): The tool creates `output_dir` with `os_mkdirs`. This is the build's first write, so a build that an earlier phase refuses creates nothing, not even an empty `output_dir`. Later writes create their own parent directories.
6. **Generate derivatives** (`generate_derivatives` -> `derivative_renderer_generate`): A second `job_run` phase owns one media item per job. It skips each non-empty derivative at least as new as its source and skips an original only when its size also matches. For stale image output, the job reads the source into a copy of its worker's scratch arena, decodes it, and applies EXIF orientation, in place for an upright source. For stale video output, it decodes the retained auto-rotated frame. The decode is scaled down in the DCT to the smallest eighth that leaves every stale derivative's source region at least twice the derivative in each dimension, so a large photo decodes a fraction of its pixels. Before decoding, each job reserves the most bytes it holds at once, while it decodes and orients the source, scaled pixels and a progressive JPEG's full-resolution coefficients included, or while it writes its largest stale derivative, from one process-wide decode budget, and holds the reservation until it releases the decoded image, so decode memory is bounded by the budget rather than by the worker count. Once a source or frame is decoded, the job gives its pages back with `arena_discard` when it is 16 MiB or more, before orienting it. It loops over the ordered derivative definitions, applies each definition's center-crop policy and bounding box, encodes the result as JPEG into a copy of its worker's scratch arena, writes it with one `os_write`, so an encode failure writes nothing, and copies the original source. Each video job removes its temporary frame when it finishes, best effort: a frame whose unlink fails stays marked for the build's final cleanup.
7. **Render pages** (`render_album_pages` -> `page_renderer_render_pages`): A third `job_run` phase renders each album through the configured album template into its result slot, in the perm of the worker that renders it. Each render reads the complete immutable album set and uses the current album's `path_to_root` to turn output-root-relative URLs into page-relative URLs.
8. **Write** (`write_generated_site` -> `site_writer`): The tool writes the album pages first. Then it renders and writes each aggregate template and copies the discovered static files into `output_dir`, each at its path below `static_dir`. Writes truncate their destination in place, so this phase is not transactional at any level: it does not remove outputs that are absent from the current manifest, a late template failure can leave new, old, and stale files together, and an interrupted write can leave one output truncated. The recovery for all three is to delete the output directory and build again.

## Modules

### Domain (`src/domain`)

- [gallery_config](src/domain/gallery_config.h): Defines `GalleryConfig`, `GalleryDerivative`, and the config load and print operations. The arena the caller passes holds the copied values that the load operation validates and normalizes. The load operation rejects a key outside the schema, at the top level or in a derivative or `sort` table, and applies optional defaults. Later phases use the resolved config without changing it. `base_url` needs an `http://` or `https://` scheme and a non-empty host. The scheme comparison is case-insensitive. The module removes trailing `/` characters from `base_url` and each configured directory. It also validates template names, the ordered named derivative table and the `sort` table, and builds the derivative recipe.
- [album](src/domain/album.h): Defines `Album`, one generated page and its immediate contents. The album, its strings, its media records, and its pointer arrays live in the arena the skeleton was built in. The album records its raw source directory, generated slug path, parent, immediate children and media, recursive cover and item count, output path, URL, depth, and relative path to the output root.
- [media_item](src/domain/media_item.h): Defines `MediaItem`, one JPEG or MP4 source, and `MediaDerivative`, one configured output for that item. The serial skeleton pass fills source, derivative-array, original, and URL fields. The probe phase fills only scalar duration, orientation, and dimension fields. The media derivative array stays aligned with `GalleryConfig.derivatives`. `media_item_has_extension` and `media_item_is_video_path` are the one ASCII-case-folding extension test that the skeleton pass and the video-tool check both use to classify a source.
- [manifest](src/domain/manifest.h): An insertion-ordered map from `output_path` to `source_label`. The tool creates it before it renders page templates or writes output files. A 4-ary hash trie keyed by the ASCII-folded path lets `manifest_add` reject a duplicate: each level takes the next two bits of the path's 64-bit FNV-1a hash, so a lookup descends O(log n) levels in expectation, and the trie never moves an entry it has placed. Two paths that differ only in ASCII case are duplicates. Other bytes, including non-ASCII bytes, compare exactly. `manifest_find_prefix_collision` finds a path that is a `/`-delimited prefix of another path. For example, it finds the conflict between `a/b` and `a/b/c`, and between `A/b` and `a/b/c`.

### Build (`src/build`)

- [template](src/build/template.h): `template_render_file` uses mustache4c to render a template with a `TemplateContext`. Template names must be safe relative paths, and partial names use a restricted identifier grammar. Each render has three limits. The partial-expansion limit stops a partial that includes itself, so every render terminates. The output-byte limit bounds the output buffer. The distinct-partial limit bounds the compiled-partial cache. The limits do not bound the total memory of a render. Templates are trusted input, and a template's nested sections use memory in proportion to the iterations that they request. All interpolation is HTML-escaped; unescaped Mustache interpolation is rejected. A media node resolves configured derivative names dynamically, so a new `xl` definition becomes `xl.url`, `xl.width`, and `xl.height` without a C model change.
- [album_scanner](src/build/album_scanner.h): Owns the serial skeleton pass and the first parallel phase. `album_scanner_build_skeleton` creates albums and media from sorted source paths in the caller's arena, plans their output, and completes their relationships. `album_scanner_probe_media` fills only the scalar fields below the probe boundary and creates one temporary JPEG per video in the frame slot the build allocated for it. `album_scanner_remove_video_frames` removes the frames a slot still marks as created.
- [job](src/build/job.h): Defines `job_run`, which connects the three parallel phases to the worker pool. It gives each job a log of its own, in the share of the phase's scratch that its worker owns, reports synchronized progress, and merges the failed jobs' logs in job order, at most twenty distinct ones plus a count of the remainder. The caller can name the text that starts each job's own name at the end of a line. Two jobs whose lines differ only after that text are one failure. Each job writes its results into its own caller-owned slot.
- [output_path](src/build/output_path.h): Reports a generated output path that exceeds either output-path limit, with one wording for every producer, and builds the `../` prefix from an output file back to the output root. `album_scanner`, `manifest_builder`, and `site_writer` share it.
- [manifest_builder](src/build/manifest_builder.h): Builds the [manifest](src/domain/manifest.h) before the tool writes a generated file. `manifest_builder_populate` records every album page, derivative, original, aggregate, and static file output. It rejects duplicate paths, nested paths below an output file, outputs at or below `input_dir`, `templates_dir`, or `static_dir`, and outputs that overwrite build inputs. `manifest_builder_check_output_dir` rejects an `output_dir` at or below any of the three roots before the build walks an input tree, whether or not `output_dir` exists yet. `manifest_builder_populate` then checks the directories below `output_dir`. The duplicate and nesting checks fold ASCII case. Build inputs include the config, sources, static files, and every file below `templates_dir`, which includes partials. The root and input checks compare device and inode values. They detect alternate spellings, hard links, symlinks, and case-insensitive paths to the same file or directory.
- [derivative_renderer](src/build/derivative_renderer.h): The second parallel phase. `derivative_renderer_generate` checks each named derivative's freshness, decodes and orients source images or retained video frames once, applies data-driven crop and fit rules, writes every stale derivative, and copies originals. Derivative paths contain the output-affecting recipe and append the derivative name to a full-filename slug that includes the source extension, while originals use a stable namespace.
- [page_renderer](src/build/page_renderer.h): The third parallel phase. `page_renderer_render_pages` renders each album into a per-index result slot, in the perm of the worker that renders it. Each render reads the complete album set through a deeply `const` template context.
- [site_writer](src/build/site_writer.h): Writes the generated gallery from the rendered results. It writes each album page first. Then it renders and writes aggregate templates and copies enumerated static files. It stops at the first output that fails to render, write or copy.

### Formats (`src/formats`)

- [image](src/formats/image.h): Buffer-to-buffer JPEG probing, RGB decoding scaled in the DCT, EXIF-orientation transforms, linear-light resizing, zero-copy crop views, bounded JPEG encoding, and aspect-ratio calculations. The module wraps libjpeg-turbo for probing and decoding and stb for resizing and encoding, and performs no filesystem I/O.
- [exif](src/formats/exif.h): Reads the IFD0 orientation from JPEG EXIF data. Missing or malformed metadata falls back to top-left orientation.
- [html](src/formats/html.h): HTML escaping of untrusted text for generated markup.
- [toml](src/formats/toml.h): TOML text validation at the boundary where parsed bytes become owned text. `toml_require_known_keys` is the shared unknown-key check: it reports each table key that a loader's list of known keys does not contain.

### Runtime (`src/runtime`)

- [memory](src/runtime/memory.h): Keeps what the out-of-memory report prints: the command's log, and for each thread the file it works on and its job's log, which `memory_oom_set_log` and `memory_oom_set_job` name.
- [pool](src/runtime/pool.h): Runs indexed jobs through a bounded pthread pool. It completes all jobs, even if one job fails. It joins all workers before it returns. Each job learns the index of the worker running it, so it can use that worker's arenas. It also gets the default worker count from the number of online CPUs. The build runs its jobs in parallel because decoding, resizing, and encoding a JPEG and running `ffmpeg` take time for each file.
- [os](src/runtime/os.h): The host interface. Every read, write, directory creation, listing, `stat` and print the program performs goes through `os_read`, `os_write`, `os_mkdirs`, `os_list`, `os_stat` and `os_print`, and each function that reaches the host takes a `struct Os*`. `os_read` takes a size limit from each caller and checks it before any read or allocation, rejects a file that is not regular and one that changes size during the read, and accepts any bytes, because it also reads media. `os_write` takes one of two sources, bytes or a file it streams, and creates parent directories. It overwrites its destination in place, through a symlink or a hard link, neither atomically nor synced: a reproducible output directory makes deleting it and rebuilding a cheaper recovery than a temporary-file protocol. A new file gets mode `0666` reduced by the process umask. `os_stat` reports identity, type, size and modification time. `os_list` closes a directory before it returns, classifies each entry after following a symlink, and skips dangling links and entries removed since it was read. `os_print` carries every byte the program prints: the log a command ends with, a rejected command line, `--help` and `--version`, the resolved config, the verbose status lines and the workers' progress line, so a test on the in-memory host sees all of it. One call writes its text whole, and a caller that prints from several threads keeps its own lock around it. `os_oom` is the remaining member, declared by `sharedstuff` and defined by each program.
- [fs](src/runtime/fs.h): The POSIX implementation of `os.h`, an object library of its own (`fram_os_posix`). The `fram` executable and every unit test registered without `OS memory` link it. Its `struct Os` has no state.
- [walk](src/runtime/walk.h): Finds and sorts matching regular files in a directory tree, through `os_list` and `os_stat` only, so it behaves the same on every host. It follows symlinked directories but walks each directory once, by device and inode. A cycle or a second alias of a directory is skipped, so an aliased directory publishes one copy of its media. A caller can also name one directory to leave out, which the walk records as already visited, so every path to it is skipped. The build leaves out `output_dir`. The walk visits entries in byte order and each symlinked directory only after every real one, so the listed path is the one without a symlink when one exists. Only one directory is open at a time, and a level of the recursion keeps no path buffer in its frame.
- [proc](src/runtime/proc.h): Runs one argument vector without a shell and captures bounded process output. It drains standard output and standard error together so neither pipe can block the child. A failed exit's reason ends with the newest standard-error bytes that fit, because the child's last line usually names the cause, with each run of line breaks shown as `; ` so the error stays one line. The copy of the argument vector the spawn takes lives in the caller's scratch arena.
- [video](src/runtime/video.h): Wraps `ffprobe` duration probing and `ffmpeg` auto-rotated JPEG extraction. Each video has a frame slot: a temporary path the build allocates before the probe, which `video_extract_frame` fills with `mkostemp`, and a flag that marks a frame as created until `video_frame_remove` removes it.

### Core (`src/core`)

- [ascii](src/core/ascii.h): Locale-independent ASCII byte classification.
- [error](src/core/error.h): Turns an `errno` into text, thread-safely. Diagnostics themselves go to the vendored `shared/log.h`, as `printf` formats that the compiler checks.
- [fram_version](src/core/fram_version.h): The tool name and version fixed when the build tree was configured.
- [parse](src/core/parse.h): Parsing of text into scalar values.
- [path](src/core/path.h): Path construction and validation. `path_is_below` finds the part of a path below a root by textual comparison. `manifest_builder` plans each static file output from it, and `site_writer` copies the static file to the same path below `output_dir`.
- [path_list](src/core/path_list.h): Growable list of path strings.
- [text](src/core/text.h): Text normalization and validation.

## Cross-cutting conventions

- **Memory**: Every first-party allocation comes from an arena, a `{beg, end, limit}` triple from `sharedstuff` that frees nothing piecemeal, except decoded pixels. Each thread has two: its perm, for data that outlives the function that makes it, which a function takes as `struct Arena* perm`, and its scratch, for temporaries, which a function takes by value, so they are free again when it returns. `template_render_file`, `image_encode_jpeg` and `os_read` return their result in the caller's `perm`. The build's log has an arena of its own. A command reserves its memory once, when it starts, as one block, through `arena_reserve_halving`, and splits the block into its arenas, each of a fixed size that the input and render limits bound, as its definition in `cmd_build.c` states. When an address-space limit refuses the block, the build halves its worker count, down to one, before `arena_reserve_halving` halves every arena. A reservation costs address space only: an arena commits pages as it fills, so only the pages a build uses count against the system's memory. Where a `malloc` design would free a buffer that a worker is done with while the worker's arena lives on, `arena_discard` gives its pages back when the work that filled them costs more than faulting them in again. A source under `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) costs less to read again into the same pages, so it keeps them. An allocation that does not fit calls `os_oom`. In `fram` it prints the command's log, the log of the calling thread's job, and `fram: out of memory (in '<file>')`, naming the file the thread was working on, then exits with `EXIT_CODE_FAILURE`, so no call site checks for allocation failure. The vendored parsers keep their own `malloc`: tomlc17's allocator is replaced by one that calls `os_oom`, and a mustache4c failure that reports no reason is treated as running out of memory at the wrapper. Under the address sanitizer two arena allocations are never adjacent, so code that extends the last allocation in place uses `arena_extend`.
- **Decoded pixels**: A `struct Image` owns its pixels on `malloc`, and `image_free` is a caller's only release of one, the only place a caller frees memory. Pixels are large, transient, allocated once per file, and bounded by the decode budget. An arena gets memory back only when its region is released, so a worker's scratch would keep its largest decode committed for the rest of the build, and the budget would stop bounding decode memory. stb's working memory uses the same allocator, through its hooks in `vendor/stb/stb.c`, and libjpeg-turbo's is on `malloc` too. The encoded JPEG is in the worker's scratch arena until its one write. `image_alloc` and `image_realloc` call `os_oom` instead of returning `NULL`, so a pixel allocation that does not fit ends the build like any other, and the report names the file. The pixels get what the arenas leave. Under an address-space limit that refuses the reservation, the build halves its worker count first, and once one worker's reservation is refused too, `arena_reserve_halving` halves it once more than it must, so the pixels keep room beside it. Under a tight limit, a large decode that would fit without the arenas can still end the build out of memory, and the report names whichever decode's allocation failed, which need not be the large one.
- **Text representation**: Every first-party string is a counted `struct Str` (`vendor/sharedstuff/include/shared/str.h`) with a signed `ptrdiff_t` length, owned or borrowed alike, and a `NUL` byte is an ordinary byte to it. JPEG bytes travel as a `struct Str` too. A C string exists only where text reaches libc, POSIX or a vendored API that takes one: the POSIX host copies each path into a `PATH_MAX` buffer at the system call (`copy_cstr`), `error_system_message` fills a caller's buffer through `strerror_r`, and `video` builds each `ffprobe` and `ffmpeg` argument with `str_cstr`. Lengths, counts and pixel dimensions are `ptrdiff_t`, and convert to `size_t`, or to `int` for stb, only at those calls.
- **Text input checks**: `os_read` accepts arbitrary bytes. Text consumers such as the TOML and template readers explicitly reject an embedded `NUL`. `toml_datum_is_text` also rejects a TOML string that contains a decoded `NUL`, and `toml_require_known_keys` rejects a TOML key that contains one before it names the key. No text format gives a `NUL` a meaning, so these keep it out of the generated output without imposing a text policy on media I/O. A path holding a `NUL` fails at the `os` boundary with `EINVAL`, because a system call would read it only up to that byte.
- **Return values**: An action, and a producer that can fail, returns `0` or `-1`, and the producer writes its result through an `_out` parameter. A producer that cannot fail returns its result directly, such as a `struct Str` by value. A predicate returns `bool`. An allocation that does not fit is never a return value.
- **Diagnostic logs**: A fallible action whose caller needs a message beyond success or failure takes a final `struct Log* log` and records every error it finds there, never truncated. A callback type puts its `void* userdata` after the log. A message is one `printf` format, and the `format` attribute makes the compiler check its arguments. A `struct Str` prints through `%.*s` and `STR_FMT`. Filesystem, image, process, and video actions also take the caller's `operation`, such as `failed to read config`, and log the whole message, because only they know which path failed. An image or process action also takes the file the bytes or the child belong to, which it names at the end. A failure found inside a file names it as `(in '<file>')` through `log_error_in`. A log's arena is never one that a callee is given by value, so the log never grows into bytes a callee is using.
- **Diagnostic text**: First-party diagnostics use lowercase prose and name the failed operation first. They use single quotes for literal names and values. External messages keep their original capitalization and punctuation. A value follows the cause.
- **Path safety**: Template names must be safe relative paths. Partial names use a more restrictive identifier grammar. Media source paths retain filesystem bytes, while generated album segments and media filenames are lowercase ASCII slugs. Output-root-relative URLs are the output paths unchanged, because every byte of a slug is unreserved in a URL, and templates prefix them with the current page's `path_to_root`. Configured output names are checked before the tool joins them to `output_dir`. These text checks prevent `.` and `..` traversal. Input-root identity checks, the walks' exclusion of `output_dir`, and input-identity checks cover existing symlink aliases at their separate boundaries.
- **Input size limits**: Each `os_read` caller passes a limit for the kind of file it reads. The read checks the limit before it allocates, so an oversize file is never loaded. `album_scanner` and `derivative_renderer` read media with `IMAGE_INPUT_LEN_MAX` from `image`, `INT_MAX`, far longer than a JPEG within the pixel limit needs, which the worker's scratch holds. `CONFIG_FILE_LEN_MAX` in `gallery_config` is 1 MiB. `TEMPLATE_FILE_LEN_MAX` in `template` is 4 MiB for each template and partial. `os_write` streams an original from its file source, so it has no size limit.
- **Path limits**: The tool checks each generated output path against two limits. `OUTPUT_PATH_RELATIVE_LEN_MAX` limits the complete relative path. `FILENAME_LEN_MAX` limits each path segment. `output_path_check_limits` reports either failure the same way for every producer.
- **Host access**: Nothing in `src/` outside `runtime/fs.c` makes a system call on a path or prints through `stdio`, apart from the video code: `proc` and `video` spawn `ffprobe` and `ffmpeg`, and `video` creates, inspects and removes the temporary frame `ffmpeg` writes, on the real host whichever host an executable links. A fake would have to emulate `ffmpeg`'s file output, so the video tests keep their fake tools on the POSIX host. Each executable chooses its host when it is linked. `fram` links `runtime/fs.c`, and so does every unit test registered without `OS memory`: the POSIX layer's own tests (`fs`, `os_posix`), the test-support test and the tests that run processes or decode video (`proc`, `video` and the `*_video` tests), which need the real host, the suites not yet converted to the in-memory host (`cli`, `cli_dispatch`, `cmd_config`, `gallery_config`, `template`), and the tests that reach no host. The tests registered with `OS memory` link [tests/os_memory.c](tests/os_memory.c), an in-memory tree with symlinks, hard links, a modification-time counter, injected failures and captured output. A new test links it too, unless it needs a fact only a real filesystem has or starts a process. A link-time choice needs no table of function pointers, because no executable needs two hosts. `src/runtime/test_os.c` builds once against each host and runs the same cases, so every property a test relies on in the in-memory host also holds on POSIX. The workers' progress line is one `os_print` call under a lock of its own, which keeps it whole across workers.
- **Parallel ownership**: No arena or log is shared between two jobs running at once. Each job uses the arenas of the worker running it, and each worker has its own. A page job renders into its worker's perm, and every other job allocates only in a copy of its worker's scratch. The serial skeleton pass and the main thread perform every allocation into the main thread's perm, the frame slots included, before the phases that use them. A worker receives immutable shared context and owns one index in each mutable result array. `pool_run` joins all workers before the main thread reads those slots, and the rendered pages stay in the worker arenas until the write phase.

## Design decisions

This section records each design decision and the reason for it. Read it before you change one of these designs. A change must answer the reason that this section gives.

These terms have one meaning in this section:

- **Block**: the one reservation a command makes when it starts. The command splits it into its arenas and releases it at its end.
- **Region**: a range of address space that one arena uses. Two arenas never use the same region at the same time.
- **Reserve**: to get address space from the system without memory behind it.
- **Commit**: to make reserved pages usable. The system charges committed pages against its memory limit.
- **Scratch**: a thread's arena for temporary data. A function gets it by value, as `scratch`.
- **Perm**: a thread's arena for data that must stay after the function returns. A function gets it by pointer, as `perm`.
- **Log**: a `struct Log`, the error lines a command prints once. It grows in an arena of its own, which no callee gets by value.

### Memory: arenas only

**Decision.** All first-party memory comes from arenas. Decoded pixels are the one exception, with a decision of their own below. An arena is two pointers, `beg` and `end`, into one region, and a third, `limit`, when the region is reserved and commits pages on demand. The code never frees one allocation. A scratch goes by value, so its memory is free again when the function returns. A perm goes by pointer.

**Reasons.**

- No comment must say who frees a buffer. Only a `struct Image` documents its release.
- The bounds check is one comparison that cannot overflow.
- A `Buf` can grow in place when it is the last allocation.
- An arena is one region, not a list of chunks, so a callee that allocates in a by-value copy leaks nothing.

### Memory: a perm and a scratch for each worker

**Decision.** Each pool worker has its own perm and its own scratch. A job uses the arenas of the worker that runs it. A page job renders its page into a copy of the perm. The job keeps the copy only when the render succeeds. Every other job allocates only in a copy of its worker's scratch. The serial skeleton pass allocates the albums, the media records, and the frame slots in the main thread's perm before the parallel phases start. The build log has an arena of its own, and `job_run` splits the main thread's scratch into one share of job logs per worker.

**Reasons.**

- A bump arena is not thread-safe. Separate arenas need no lock.
- A failed render leaves nothing in the arena.
- A job reads its source or frame with `IMAGE_INPUT_LEN_MAX` as the size limit, which the worker's scratch holds, so a source that grew past it since the walk fails that job alone.
- Arenas belong to threads, not to albums or media items, because a gallery has thousands of media items, and a cap large enough for the largest one, once for each item, needs too much address space.

### Memory: reserve, then commit on demand

**Decision.**

1. The build reserves its block with `PROT_NONE` when it starts, and releases it at its end. The reservation costs no memory.
2. An arena commits more pages only when an allocation does not fit in the committed part. The fast path stays one bounds check. The slow path commits pages a mebibyte past what the allocation needs, up to the end of the region.
3. Each arena has a fixed size, derived from the input and render limits that bound what it holds: `IMAGE_INPUT_LEN_MAX`, `IMAGE_ENCODED_LEN_MAX`, the template and render limits, and the expansion limit. Every thread's perm is `BUILD_PERM_LEN` (4 GiB) and every thread's scratch `BUILD_SCRATCH_LEN` (3 GiB). The log is `BUILD_LOG_LEN` (256 MiB), and the main thread's scratch holds one more `BUILD_LOG_LEN` share of job logs for each worker and one for `job_run`. The sizes are in `cmd_build.c`, each with the bound that justifies it. The only input the reservation depends on is the worker count.
4. The build runs the requested number of workers, or one per online CPU by default, and at most `BUILD_WORKER_PER_CPU_MAX` (four) per online CPU, and reserves arenas for that many before it reads any input. `pool_run` starts no more threads than a phase has jobs. The verbose status lines name the count the build has arenas for.
5. If the system refuses the block, `arena_reserve_halving` halves it, and every arena with it, up to six times, and halves the first size that fits once more, so `malloc` in the vendored libraries, decoded pixels and thread stacks keep room. If even the smallest size is refused, the build ends with the out-of-memory report.
6. A worker's arenas outlive its jobs, so `arena_discard` gives back the pages of a buffer a worker is done with: a source of `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) or more once it is probed and again once it is decoded, and a template's bytes once it is compiled.

**Reasons.**

- Each arena's size comes from the limits rather than from an estimate of what the inputs need. An estimate takes a budget read from `RLIMIT_AS`, per-region estimates from the media counts, path lengths and the largest JPEG, least and most sizes and sized refusals, helps only under `ulimit -v`, and can be wrong in both directions: a few hundred album pages of a few mebibytes leave one worker without room, a deep album tree outgrows its page estimate after the reservation has succeeded, and a budget of half the address-space limit roughly doubles the limit a gallery needs.
- A fixed reservation from the limits holds every input the limits accept. A build without an address-space limit runs out of memory only when the system does.
- An arena commits its pages on demand, not all at once, because strict overcommit charges every committed page against the system's limit, and Linux ignores `MAP_NORESERVE` under it.
- Reserving costs system time in proportion to the size, about 1 µs per GiB to reserve and as much to release, on macOS. Sizes from the limits keep that to a fraction of a millisecond, where a reservation as large as the address space allows would cost tens of milliseconds on every build.
- One reservation at startup, for the capped worker count, puts the whole layout in one function. A gallery with fewer jobs than workers reserves arenas that some workers never use, which costs address space only.
- A by-value copy cannot tell the original what it committed, so each job's copies commit their pages again, about one system call per job. Sharing a committed end between copies would skip the poisoning that `arena_commit` does under the address sanitizer, so bytes a dead copy unpoisoned would be handed out unpoisoned. Committing a worker's first step once, before its jobs, would save about 1% of system time for a new mechanism, so it is not done.
- A smaller source is not given back, because the worker's next job reads into the same pages, and faulting them in again would cost more than reading it. The derivative phase gives a large source back once it is decoded and its orientation read: the pixels hold all the derivatives need, and the worker's scratch outlives the job, so the source's pages would otherwise stay resident beside other workers' decodes and beside a rotated photo's second decoded-size buffer. A template's bytes are given back because nothing in the render reuses their pages.

**Cost.** Under a `ulimit -v` below the full reservation, the build runs with halved arenas, which hold less than the limits allow, and a large input then ends in the out-of-memory report rather than a sized refusal. Under such a limit only correctness counts: the build succeeds or ends with that report, and never writes wrong bytes. The decoded pixels decision below gives the cost for a large decode.

### Out of memory exits the process

**Decision.** When an arena cannot commit more pages, it calls `os_oom`. Each program defines `os_oom`. The `fram` version does these steps:

1. It prints the errors that the command's log and the thread's job log have.
2. It prints `fram: out of memory` and the name of the file that the thread was processing. A command names its log with `memory_oom_set_log`, and a job names its file and log with `memory_oom_set_job`.
3. It exits with `EXIT_CODE_FAILURE`.

**Reasons.**

- `fram` is a CLI tool. Its output directory is easy to make again.
- No call site checks for allocation failure.
- The report names the file the thread was working on, which a user needs to find the input that failed.
- A failed pixel allocation ends the build like any other, not one job, so the build has one failure policy and no `NULL` check after a pixel allocation.

**Vendored code.** mustache4c, tomlc17 and libjpeg-turbo keep `malloc`. Each wrapper changes their allocation failure into a call to `os_oom`. The JPEG codec reports a failed allocation as a fatal error, which `image_decode` turns into `os_oom`. The stb hooks allocate through `image_alloc` and `image_realloc`, which call `os_oom` when `malloc` fails.

**Limit.** The exit skips the build's cleanup of temporary video frames, and it does not stop a running `ffmpeg` child, as Ctrl-C and `kill` leave them, because `src/` installs no signal handlers. A temporary directory for each build would make that one directory to remove.

### Decoded pixels stay on `malloc`

**Decision.** A `struct Image` owns its pixels on `malloc`: the output of `image_decode`, `image_orient`, and `image_resize_region`. libjpeg-turbo's working memory, such as its row buffers and a progressive JPEG's coefficients, is on `malloc` as well, and stb's resizer and encoder buffers use `image_alloc` through the hooks in `vendor/stb/stb.c`. `image_free` is a caller's only release of a `struct Image`, and `struct Image` is the one type whose memory a caller frees. `image_alloc` and `image_realloc` call `os_oom` instead of returning `NULL`. Everything else on the image path is on arenas: the source bytes and the encoded JPEG, in the worker's scratch arena, and every path and message.

**Reasons.**

- An arena gets memory back only when its region is released. With the decode's buffers in a worker's scratch, each worker would keep its largest decode committed for the rest of the build. At the measured peaks for a 100-megapixel 4:2:2 photo, that is up to 302 MiB for a baseline JPEG decoded at full size and 683 MiB for a progressive one, so about 2.9 GiB or 6.7 GiB with 10 workers, and a progressive CMYK one peaks at about 1.05 GiB.
- The decode budget would stop working. It counts a reservation as returned when the job calls `image_free`, but the pages would stay committed in the worker's region, so memory would be bounded by the worker count again.
- Pixels are large, transient, allocated once per file, and bounded by the decode budget. `malloc` handles that case well, and a bump arena handles it badly.
- The exception has a type, `struct Image`, and two files, `formats/image.c` and `vendor/stb/stb.c`. [STYLE.md](STYLE.md#memory-and-resources) allows `malloc` and `free` only in those two files and in the allocator wrapper that `formats/toml.c` gives tomlc17.
- Pixels do not go in a worker's region with a primitive that decommits it. Under strict overcommit `madvise(MADV_DONTNEED)` leaves the pages charged, so returning them takes `mmap(MAP_FIXED, PROT_NONE)`, and a stale by-value copy whose `end` lies above the mark would then write into `PROT_NONE` pages and crash, where `arena.h` documents that a copy which makes pages usable again changes nothing. stb's encoder hooks take no context, so the target arena would be a `_Thread_local` pointer, and the budget would have to wait for the decommit, all to remove about eight `malloc` and `free` calls from one module. `arena_discard` avoids the hazard: it maps the pages anew readable and writable, so they stay usable, and it applies to a buffer the caller names, not to an arena's free space.
- Pixels get no fixed share of the reservation. The largest decode, of a 100-megapixel progressive CMYK JPEG, peaks at about 1.05 GiB, and the budget leaves room for two, so a fixed share would refuse any build under a `ulimit -v` below about 3 GB, galleries of 24-megapixel photos included.
- The decode budget does not scale with what the arenas leave, because that would change how many decodes run at once on a limited system and would need a second planner, keyed on decode memory.

**Cost.** The pixels get what the arenas leave. Under a tight `RLIMIT_AS`, a large decode that would fit without the arenas can end the build out of memory, and the report names whichever decode's allocation failed, which need not be the large one.

**Revisited** with libjpeg-turbo's scaled decode, which bounds a baseline JPEG's pixels by its largest stale derivative rather than by `IMAGE_PIXEL_COUNT_MAX`: a 100-megapixel 4:2:2 baseline JPEG decoded at an eighth peaks at 20 MiB. Pixel scratch for each worker, sized from the configuration, is still not reasonable. A progressive JPEG keeps its coefficients at full resolution at every scale: the same photo progressive peaks at 401 MiB at an eighth, and a progressive CMYK one at about 800 MiB. A derivative can be configured up to `IMAGE_DIMENSION_MAX`, which leaves some galleries no reduction at all. And the scale moves in eighths, so a decode can exceed twice its largest derivative by an eighth of the source in each dimension. The scaled decode instead makes the budget count bytes, so a decode reserves what it holds rather than its source's pixel count.

### Address sanitizer coverage in arenas

**Decision.** In a build with the address sanitizer, an arena over a reserved region poisons the pages it commits. `arena_alloc` unpoisons only the bytes that it returns. In such an arena it leaves a poisoned gap before each allocation, so two of its allocations are never adjacent. An arena over memory the caller owns has no gap.

**Reasons.** ASan cannot see an overflow that stays inside one large mapping. Without this step, the debug preset loses most of its overflow checks.

**Limit.** The arena does not poison scratch memory again when a function returns. ASan does not find a use of scratch memory after the function that owned it returns. A job's copy of its worker's arenas poisons the free space again when it commits, so a job that allocates past the usable part does not read what an earlier job allocated there. `arena_release` leaves the shadow to the sanitizer's `munmap` interceptor, which clears it, so a later mapping at the same addresses inherits no poison.

### A log must not grow in a borrowed arena

**Decision.** A `struct Log` and its `Buf` live in an arena that no callee gets by value. The build gives each log an arena of its own, or an arena that everything using it reaches by pointer.

**Reason.** A `Buf` grows in place. Assume that a callee gets a copy of the same arena by value. The callee then allocates the same bytes that the log grows into. The two overwrite each other with no crash and no ASan report.

**How to apply.** [STYLE.md](STYLE.md#memory-and-resources) states the rule. Treat a break of it as a bug, not as a style problem.

### Counted strings with signed lengths

**Decision.** Every first-party string is a `struct Str`, a pointer and a `ptrdiff_t` length. JPEG bytes travel as a `struct Str` too. Every length and count is `ptrdiff_t`. So is every pixel dimension. A value changes to `size_t` only at a libc, POSIX, or vendored call, or in a C buffer that goes to one, such as the `PATH_MAX` buffer of `copy_cstr`. A pixel dimension changes to `int` only at a call into stb, and to libjpeg-turbo's unsigned `JDIMENSION` only at a call into it. Each such change asserts that the value is not negative, unless that holds by construction. A C string exists only where libc, POSIX, or a vendored API needs one.

**Reasons.**

- Subtraction cannot wrap to a very large value.
- A `NUL` byte is an ordinary byte, so no code calls `strlen` on owned text.

**Kept on purpose.** `os_read` accepts any bytes, because JPEGs contain `NUL` bytes. The config and template readers reject a file that contains a `NUL`, and `toml_datum_is_text` rejects a TOML string that contains `\u0000`, and `toml_require_known_keys` a TOML key. This is a product decision, and the string type does not force it.

### A growable log, and every error in one run

**Decision.**

- Each fallible function whose caller needs a message takes `struct Log* log` as its last parameter. A callback type puts its `void* userdata` after the log. The log keeps every error with no truncation.
- The build continues after an error while the next phase still has something to check.
- The build stops before its first write if the log has an error.
- The log records the phases' errors in phase order, and the errors of one phase in walk order or in job order. The order does not change with the number of workers.
- A parallel phase prints at most twenty distinct job errors and a count of the others.
- A rejected config value keeps its default, so one bad value causes one error. A `derivatives` table is kept only when every entry is valid, and a rejected `base_url` falls back to empty.

**Reasons.**

- A user sees all errors in one run.
- No message buffer has a fixed size, so no message is truncated, and none is ordered to survive truncation.

### Log messages use checked format strings

**Decision.** A log call takes one `printf` format and its arguments. `log_error`, `log_error_in`, `log_error_v`, `buf_printf`, and `buf_vprintf` have `__attribute__((format(printf, ...)))`. A `struct Str` prints with `%.*s` and `STR_FMT`. An integer prints with the conversion of its type. An example:

```c
log_error(log, "failed to read template: %s ('%.*s')", reason, STR_FMT(template_path));
```

**Reasons.**

- Each message is one sentence in the source. A reader can read the wording as one line, and `grep` finds it.
- The compiler checks the number and the types of the arguments against the format. `-Wformat=2` is on for first-party code and for `sharedstuff`.
- `%td` prints a `ptrdiff_t` with no cast. Only a string needs a helper, `STR_FMT`.
- A first-party function that formats a message gets the same check: it has the attribute.

**Costs and limits.**

- `STR_FMT` evaluates its argument two times. Give it a variable or another lvalue. Put a computed string in a variable first.
- A string longer than `INT_MAX` bytes trips an assertion at run time, because the `%.*s` precision is an `int`.
- The attribute is the only guard on the code that reads the arguments. Each formatting function measures the text with `vsnprintf`, allocates it, and writes it with a second `vsnprintf`: `buf_printf` and `buf_vprintf` in `sharedstuff`, which grow the buffer in place, and the first-party functions that format their own message. The measured `int` length becomes a `ptrdiff_t` only in those functions.
- A first-party variadic function does not pass its `va_list` on to `log_error_v` or `buf_vprintf`. Clang's analyzer, version 22, targeting AArch64 Linux, where a `va_list` is a structure, reports a false `va_end` on an uninitialized list when it does. The function starts its list once for each `vsnprintf` pass instead, as `buf_printf` does: `record_error` in `cli.c`, `build_verbose` in `cmd_build.c`, `render_fail` in `template.c`, and `test_log_is` in `tests/test_support.c`.
- `%.*s` stops at a `NUL` byte. The text readers reject a `NUL` in config and template content, `toml_datum_is_text` and `toml_require_known_keys` reject one in TOML strings and keys, and the `os` boundary rejects one in paths, so a message gets no `NUL` from text input. No message prints JPEG bytes. A path that holds a `NUL` is quoted only up to that byte in its `EINVAL` message. The contract test for this shows the result.
- An `off_t` has no conversion of its own. It prints with `%jd` and a cast to `intmax_t`.
- `clang-format` breaks a string literal that passes the column limit. Some long messages are in two literals.

### The `os` interface

**Decision.** All host access goes through `src/runtime/os.h`. `fs.c` is the POSIX implementation. `tests/os_memory.c` is an in-memory implementation for unit tests. The link step selects one implementation for each executable. A function that touches the host takes `struct Os*`, just before its arenas, and an `os_*` function takes it first. The exception is `proc` and `video`: they run `ffprobe` and `ffmpeg`, and handle the frame that `ffmpeg` writes, on the real host whichever host an executable links, so the video tests keep their fake tools on the POSIX host.

**Reasons.**

- Tests of the walk, the manifest, the derivatives, the pages, and the writes need no temporary directories.
- A stale-output cleanup could test its checks that nothing is removed outside the output root without a disk.
- A probe cache would read and write its cache through `os_read` and `os_write`.

**Condition.** Each property that a test checks against the in-memory host must have a contract test. One test source compiles into two executables: one links the POSIX host and one links the in-memory host. Both must pass. Without the contract tests, the in-memory host can accept what POSIX rejects. For example, an in-memory host could accept `file/` as a file, but POSIX returns `ENOTDIR`, and a contract test checks it.

**The writes.** `os_write` takes one of two sources: bytes, or a file that it streams in 64 KiB pieces, so `os_write` and `os_mkdirs` are the only operations that change the host, and a copy needs no operation of its own. It writes every output in place, through a symlink or a hard link at the destination, and neither atomically nor synced, so an interrupted build can leave a cut file, which the freshness check keeps until its source changes. Deleting `output_dir` and building again recovers it. `os_stat` reports size and modification time for that check.

### Seeded random tests in the normal suite

**Decision.** `tests/test_random.c` checks the parsers of untrusted file headers, the EXIF reader and the JPEG probe, on generated inputs: a well-formed EXIF segment reads back the orientation it was built with, and a mutated EXIF segment or JPEG header reads without a crash or a read past the input. It draws them from the generator in `tests/test_support`, a 64-bit linear congruential step. The seed comes from `FRAM_TEST_SEED` and is fixed when that is unset (`TEST_RNG_SEED_DEFAULT`), so every run checks the same cases. A failing check prints its seed, and setting `FRAM_TEST_SEED` to that seed replays it. A malformed seed fails the test instead of running another seed.

**Reasons.**

- The pass runs in every test preset, under its sanitizers, with no fuzzing engine.
- A fixed default keeps the suite deterministic, and a printed seed makes a failure from another seed reproducible.

**Limit.** The pass reaches only the input shapes its generators list. Searching beyond them is the work of an EXIF fuzzing harness, which the tree does not have yet.

### Where the shared code lives

**Decision.** `sharedstuff` holds the generic parts: the arena with reserve, commit, and poisoning, `Buf`, `Str`, and `Log`. Each program defines `os_oom`. Each program keeps its own `os` interface and its own arena layout. `arena_release` does not unpoison a whole region under the address sanitizer, which would write an eighth of each reservation as resident shadow. The pixel exception stays out of it.

**Reason.** These parts do not depend on what a program does.
