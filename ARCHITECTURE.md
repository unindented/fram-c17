# Architecture

`fram` is a small C17 static photo and video gallery generator. It reads a TOML config, a directory tree of JPEG and MP4 media, and Mustache templates, and it writes the generated files to a directory that the gallery owns. This document describes the code: the directories and their dependency rules, the build pipeline, the job of each module, the conventions that every module follows, and the design decisions behind them. [README.md](README.md) covers usage and template behavior, [BUILD.md](BUILD.md) covers the CMake build, and [STYLE.md](STYLE.md) covers the code style.

## Layout

- [src/app/](src/app/): The process entry point, the command-line interface, and the command boundaries.
- [src/build/](src/build/): Build orchestration, the parallel phases, templates, manifest population, and output writing.
- [src/domain/](src/domain/): Gallery configuration, albums, media items, the build cache, and the output manifest.
- [src/formats/](src/formats/): The boundaries for HTML, JPEG, WebP, EXIF, and TOML data.
- [src/runtime/](src/runtime/): The host interface and its POSIX implementation, the directory walk, process and video services on the real host, the out-of-memory report, and the thread pool.
- [src/core/](src/core/): Small reusable primitives, containers, diagnostics, text, and path logic.
- [tests/](tests/): The golden test suite, the shared unit-test support library [tests/test_support.h](tests/test_support.h), and the in-memory host [tests/os_memory.h](tests/os_memory.h). The fixture gallery [tests/fixtures/gallery](tests/fixtures/gallery/), which mixes JPEG and MP4 sources, has its expected output in [tests/expected/gallery](tests/expected/gallery/).
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project entry point and the supported CMake configurations.
- [cmake/](cmake/): CMake profiles, quality tools, packaging, the golden test driver, and reusable cross toolchains. [BUILD.md](BUILD.md) documents their boundaries and policy.
- [vendor/](vendor/): The bundled dependencies `sharedstuff`, `copt`, `tomlc17`, `libjpeg-turbo`, `libwebp`, `stb`, `mustache4c`, and `acutest`.

### Dependency rules

First-party dependencies point inward:

- `app` can depend on every other directory.
- `build` can depend on `domain`, `formats`, `runtime`, and `core`.
- `domain` can depend on `formats`, `runtime`, and `core`.
- `formats` and `runtime` can depend on `core`.
- `core` depends on no other first-party directory.

A module can skip layers, and modules in the same directory can depend on each other. Vendored code is outside this hierarchy. A first-party include starts at the `src/` root, for example `#include "core/ascii.h"`, so every include names the directory that it depends on.

## Terms

These words have one meaning everywhere in this document:

- **Arena.** A `struct Arena` from `sharedstuff`. It has two pointers, `beg` and `end`, into one region. When the region is reserved and the arena commits pages on demand, it has a third pointer, `limit`. An arena never frees an individual allocation.
- **Perm.** A thread's arena for data that lives longer than the function that makes it. A function takes it by pointer, as `struct Arena* perm`.
- **Scratch.** A thread's arena for temporary data. A function takes it by value, as `struct Arena scratch`, so its allocations are free again when the function returns.
- **Log.** A `struct Log`: a growable buffer with one line for each error, and an error count. A command prints its log once. A log grows in an arena of its own, or in an arena that all users of the log get by pointer, so no callee gets the log's arena by value.
- **Block.** The one reservation that a command makes when it starts. The command splits the block into its arenas, and releases the block when it ends.
- **Region.** A range of address space that one arena uses. Two arenas never use the same region at the same time.
- **Reserve.** To get address space from the system without memory behind it.
- **Commit.** To make reserved pages usable. The system charges committed pages against its memory limit.
- **Build.** One run of `fram build`. [BUILD.md](BUILD.md) documents the CMake build of `fram` itself.
- **Keep.** To leave an existing derivative or copied original in place, without a new encode or copy, because it is current. The output writer marks a kept output in the manifest, as it marks a written output.

## Entry point and commands

[main.c](src/app/main.c) defines `main`. `main` only calls `cli_dispatch` with the POSIX host, so the unit tests can link the code that maps an action to an exit code. These are the other modules in `src/app`:

- [cli](src/app/cli.h): `cli_parse` parses the command line into a subcommand and one of four actions: version, help, run, or error. It never prints and never exits. `cli.c` is the only translation unit that defines `COPT_IMPL`.
- [cli_dispatch](src/app/cli_dispatch.h): `cli_dispatch` calls `cli_parse`. For the run action, it calls the subcommand's function. For each other action, it prints one text through `os_print`: the diagnostic for the rejected command line, the `--help` text, or the `--version` text. It formats these texts in a reservation of its own, and releases that reservation when the command or the print returns. A command reserves its own memory.
- [cmd_build](src/app/cmd_build.h): `cmd_build_run` generates the gallery, as [Build pipeline](#build-pipeline) describes.
- [cmd_config](src/app/cmd_config.h): `cmd_config_run` loads `fram.toml` and prints the resolved config.
- [exit_code](src/app/exit_code.h): Every command returns an `enum ExitCode`: `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).

## Build pipeline

A build runs in two layers:

- `cmd_build_run` is the outer boundary. It resolves the worker count, and reserves the build's memory once, as one block of address space. It splits the block into arenas: the build's log, and a perm and a scratch for the main thread and for each worker. It calls `cmd_build_execute`, and then releases the block. If the build fails, it prints the build's log to `stderr` once, through `os_print`.
- `cmd_build_execute` owns the `BuildState`. It runs the twelve phases below in sequence, with the arenas that it gets, and it prints the verbose status lines. It runs the first phase itself, and `run_build_phases` runs the other eleven.

The phases call these modules in `src/build`:

- `album_scanner` builds the album tree and probes the media.
- `manifest_builder` plans the output paths.
- `derivative_renderer` generates the media outputs.
- `page_renderer` renders the album pages.
- `site_writer` writes the generated gallery.
- `output_pruner` removes what the build did not write.

Every change under `output_dir` goes through `output_writer`, which does not make the changes in a dry run.

### Phases

The phase order follows the data dependencies. A template can read media dimensions, album covers, and descendant counts, and none of these values is complete until the build creates the album tree and probes every media item. Thus the build makes every arena-owned string serially, and then runs separate parallel phases to probe media, generate derivatives, and render pages.

1. **Load inputs** (`load_build_inputs`): `gallery_config_load` reads and validates `fram.toml`. Each configured read root must be a usable directory, and the build reports the cause when one is not. The build skips the optional static root when a gallery does not have one. Then, before the build walks an input tree, `manifest_builder_check_output_dir` rejects an `output_dir` that is at or below `input_dir`, `templates_dir`, or `static_dir`, and an `output_dir` that contains one of them or the config file. Then `walk_files_with_suffixes(paths, input_dir, output_dir, suffixes, ...)` finds and sorts the JPEG and MP4 sources in one walk, and the static files in a second walk. Each walk of an input tree excludes `output_dir` by its identity, so a symlink into the output tree cannot send the outputs of an earlier build back in as inputs. When an MP4 exists, the phase checks once for `ffmpeg` and `ffprobe`.
2. **Build album skeleton** (`build_album_skeleton` -> `album_scanner_build_skeleton`): The build walks the sorted media paths serially. It creates the root album and the nested albums, and allocates every string and media record in the main thread's perm. It converts the generated album and media output names to slugs, plans the page and media paths, and builds the flat media array. It then fills the parent, child, cover, and count relationships. Before it selects covers, it reverses the child array and the media array of each album when the configured sort order is descending. The flat media array and the album array stay in source order. A raw directory basename becomes an album title, with each `_` shown as a space. The phase also allocates one video-frame slot for each media item, with its temporary path for a video, and one rendered-page slot for each album.
3. **Load the build cache** (`load_build_cache`): The build reads `_fram/v<n>-build-cache.tsv` below `output_dir` into the main thread's perm, unless `--force` tells it to ignore the file. The file holds what the probe of an earlier build read from each source, with the size and modification time of the source. If the file is missing, cannot be read, does not parse, or has a first line that names a different generator, the build uses an empty build cache and gives a verbose note. None of these cases is an error.
4. **Probe media** (`probe_media` -> `album_scanner_probe_media`): `job_run` runs one job for each media item. Each job stats its source into a slot of its own, and uses a binary search to look up the source in the build cache, because both lists are sorted. If a row matches the size and the nanosecond modification time, it fills the probe fields, so the job reads no source and runs no tool. Every other job probes its source:
   - An image job reads the source, probes its JPEG dimensions, reads its EXIF orientation, and calculates its presented and derivative dimensions.
   - A video job probes the duration, and selects the configured frame time, or the midpoint when that time is after the end. It extracts one auto-rotated temporary JPEG into its frame slot, and probes the dimensions of that frame.

   Each job reads into a copy of its worker's scratch arena, up to `IMAGE_INPUT_LEN_MAX`, so if a source or a frame is over the limit, only that job fails. After the job probes a source of 16 MiB or more, it gives back the pages of the source with `arena_discard`. Each job writes only to the scalar fields of its media record and to its own frame slot, and it writes its errors to its own log, in the log share of its worker. `pool_run` joins every worker before `job_run` merges the job logs into the build's log. When the build ends, on success and on failure, it removes every frame that a slot still marks as created. An out-of-memory exit skips that cleanup and leaves the current frames in `TMPDIR`, and a running `ffmpeg` child finishes its frame after the exit. Ctrl-C and `kill` leave the frames in the same way, because `src/` installs no signal handlers.

5. **Manifest** (`populate_output_manifest` -> `manifest_builder_populate`): Before the build writes a generated file, it records every output path for album pages, derivatives, originals, aggregates and static files, and it records the build cache's path last. It rejects four kinds of conflict:
   1. Two producers claim the same path, and `manifest_add` rejects the second claim. Two paths that differ only in ASCII case are the same path, because a case-insensitive filesystem, such as the macOS default, stores them as one file.
   2. One output uses `a/b` and another output uses `a/b/c`. The first path must be a file, but the second path needs a directory at `a/b`. `manifest_find_prefix_collision` finds this conflict after the build records every path. It folds ASCII case in the same way, so `A/B` and `a/b/c` also conflict.
   3. An output names an existing build input. `manifest_builder_populate` compares the device and inode of each output with the device and inode of these inputs: the config, every discovered media file and static file, and every file below `templates_dir`. The template tree includes partials and templates that no configuration names. A missing `templates_dir` adds no inputs, although in a full build the load inputs phase already rejected a missing `templates_dir`. If `templates_dir` exists but the build cannot walk it, the build fails.
   4. An output is at or below `input_dir`, `templates_dir`, or `static_dir`, even when no file exists there yet. Such an output would come back as an input: the next build would find it as media below `input_dir` or copy it as a static file below `static_dir`, and the same build could read it as a partial below `templates_dir`. In the load inputs phase, `manifest_builder_check_output_dir` already compared the device and inode of each root with the nearest existing ancestor of `output_dir`, and with each ancestor of that directory. This rejects an `output_dir` inside a root before any walk, whether `output_dir` exists yet or not. `manifest_builder_populate` then compares each root with each existing directory between `output_dir` and the output. This catches a symlink inside `output_dir` that points into a root. These comparisons detect alternate spellings and symlinked roots. A sibling directory such as `photosx` does not match, and the comparisons skip a missing root. This check runs before the input-file check, so for an output below a root, the build reports this conflict.
6. **Prepare output** (`prepare_output_dir`): The build creates `output_dir` through `output_writer_mkdirs`. This is the build's first write, so when an earlier phase stops a build, the build creates nothing, not even an empty `output_dir`. Later writes create their own parent directories. From this point, every change under `output_dir` goes through the build's `OutputWriter`. In a dry run, the writer makes no change: it creates no `output_dir`, writes no file, and records what the prune would remove instead of removing it.
7. **Write the build cache** (`write_build_cache`): The build writes the build cache through the output writer, with its first line and a row for each current source, so a deleted source loses its row. This phase runs as soon as `output_dir` exists, because the probe facts stay correct when a later phase fails. If the probe failed, the build stopped before this phase, so the previous build cache does not change.
8. **Generate derivatives** (`generate_derivatives` -> `derivative_renderer_generate`): A second `job_run` phase runs one job for each media item. The job keeps each derivative that is not empty and is at least as new as its source. It keeps an original only when the size of the original also matches. It marks each kept output in the manifest. A dry run marks each stale derivative and original as written, and makes none of them, because if a dry run decoded a source only to discard its derivatives, a dry run of an empty `output_dir` would be as slow as a build. This is the one place outside the output writer that asks whether the build is a dry run.
   - For a stale image output, the job reads the source into a copy of its worker's scratch arena, decodes it, and applies the EXIF orientation, in place for an upright source.
   - For a stale video output, the job decodes the retained auto-rotated frame. If the build cache gave the probe facts for the video, the job first extracts the frame, so a cached video whose derivatives are current never runs `ffmpeg`.

   libjpeg-turbo scales the decode down in the DCT to the smallest eighth that leaves the source rectangle of every stale derivative, the whole source or its center crop, at least twice the size of the derivative in each dimension, so a large photo decodes only a fraction of its pixels. Before it decodes, each job reserves from one process-wide decode budget the maximum number of bytes that it holds at one time. This maximum is the memory either to decode and orient the source, including the scaled pixels and the full-resolution coefficients of a progressive JPEG, or to write its largest stale derivative. The job holds the reservation until it releases the decoded image, so the budget, not the worker count, bounds the decode memory. After the job decodes a source or a frame of 16 MiB or more, it gives back the pages of the source or frame with `arena_discard`, before it orients the image. The job loops over the ordered derivative definitions, and applies the center-crop policy and the bounding box of each one. It encodes each result as a lossy WebP into a copy of its worker's scratch arena, and writes the result through the output writer in one write, so an encode failure writes nothing. It then copies the original source. Each video job removes its temporary frame when it finishes, as a best effort. If the unlink of a frame fails, the frame stays marked for the final cleanup of the build.

9. **Render pages** (`render_album_pages` -> `page_renderer_render_pages`): A third `job_run` phase renders each album through the configured album template into its result slot, in the perm of the worker that renders it. Each render reads the complete immutable album set, and uses the `path_to_root` of the current album to change URLs relative to the output root into URLs relative to the page.
10. **Write** (`write_generated_site` -> `site_writer`): The build writes the album pages first. Then it renders and writes each aggregate template, and copies the discovered static files into `output_dir`, each at its path below `static_dir`. Each write truncates its destination in place, so this phase is not transactional at any level. A late template failure can leave new, old, and stale files together, and an interrupted write can leave one output truncated. To recover from both, delete the output directory and build again.
11. **Remove stale outputs** (`prune_stale_outputs` -> `output_pruner_prune`): After the build writes every output, it removes every directory entry below `output_dir` that is not an output and is not a directory that an output is below, so the tree holds only what the current inputs produce. The prune compares device and inode values, not names, so on a case-insensitive filesystem it does not remove the old spelling of a current output. It sorts the identities once, so each lookup is a binary search. It removes a symlink as a link and never goes into one, so it touches nothing outside `output_dir`. It also never goes into a directory that is an input root. It removes a stale directory after it removes everything in it. When it cannot list or remove a directory entry, it reports the failure and continues. It does not report again a directory that such a failure left non-empty. A failed build never gets to this phase, so it removes nothing. In a dry run, the prune finds no `output_dir` when no earlier build made one, and then it has nothing to remove.
12. **Report the dry run** (`report_dry_run` -> `output_writer_report`): This phase runs only in a dry run. The build prints to `stdout`, in one `os_print` call, a `write` or `keep` line for each output path in manifest order. Then it prints a `remove` line for each path that the prune would remove, sorted bytewise. If an output path is still unmarked, a call site wrote without the output writer. In that case the report fails, so that it does not print a list that leaves out the write.

### Errors and status output

No phase writes a failure diagnostic to `stderr` itself. The one exception is the out-of-memory report, which `os_oom` prints through `memory_oom_report` before the process exits. Every fallible function records its errors in the log that its caller gives, and `cmd_build_run` prints the build's log once.

Until the first write, the build continues after an error wherever the next phase still has something to check. One run reports every bad config key, and every album and source in the album skeleton that is past a path limit. The exception is the sub-albums and sources of an album that the build already reported, because they fail with that album. When the skeleton is complete, the run also reports every source that the build cannot probe, and every output collision. If the build recorded an error, it then stops before it creates `output_dir`. A config that the build cannot read, a rejected directory key, a root that the build cannot read or walk, a missing video tool, or a skeleton error stops the build before the next phase, because it leaves the build nothing to check.

After the first write, a failed phase is the last phase that runs. The derivative phase still generates the outputs of every other media item, and reports each item that fails. The page phase still renders every other album, and reports each album that fails. `site_writer` stops at the first album page, aggregate template, or static file that fails. The prune runs only when every write succeeded, and it reports every directory entry that it cannot list or remove.

A parallel phase gives each job a log of its own. After `pool_run` joins the workers, `job_run` appends the logs of the failed jobs to the build's log in job order: at most `JOB_ERROR_REPORT_COUNT_MAX` (20) distinct logs, then a count of the others. An album page render ends its failure message with `(while rendering album '<dir>')` or `(while rendering the root album)`, and the comparison of two logs stops at that text, so when every page repeats the same template failure, the build prints that failure once.

Verbose status is separate from diagnostics, and both go through `os_print`. The main thread writes a status line between phases. A worker prints nothing to `stderr` except its progress line and an out-of-memory report. The worker formats the whole progress line (`\r<label> n/total`, such as `probing media 3/12`) before it prints it in one `os_print` call, under a lock that `job.c` owns, so the lines of two workers never interleave.

## Modules

Each item below links to the module's header, which documents its interface. The `src/app` modules are in [Entry point and commands](#entry-point-and-commands).

### Build (`src/build`)

- [album_scanner](src/build/album_scanner.h): The serial skeleton pass and the first parallel phase. `album_scanner_build_skeleton` creates albums and media from the sorted source paths in the caller's arena, plans their output, and completes their relationships. `album_scanner_probe_media` fills only the scalar fields below the probe boundary. It fills them from the build cache when the row of a source still matches the source. Otherwise it probes the source, and for a video it creates one temporary JPEG in the frame slot that the build allocated for the video. `album_scanner_remove_video_frames` removes the frames that a slot still marks as created.
- [derivative_renderer](src/build/derivative_renderer.h): The second parallel phase. `derivative_renderer_generate` checks whether each named derivative is current, decodes and orients a source image or a retained video frame once, applies the crop and fit rules of each derivative definition, writes every stale derivative, and copies the original. A derivative path holds the recipe of the settings that affect the output, and appends the derivative name to a slug of the full filename, which includes the source extension. Originals use a stable namespace.
- [job](src/build/job.h): `job_run` connects the three parallel phases to the worker pool. It gives each job a log of its own, in the share of the phase's scratch that the job's worker owns. It reports progress under a lock, and merges the logs of the failed jobs as [Errors and status output](#errors-and-status-output) describes. The caller can name the text that comes before each job's name at the end of a line. When the lines of two jobs differ only after that text, the two jobs count as one failure. Each job writes its results into its own slot, which the caller owns.
- [manifest_builder](src/build/manifest_builder.h): Builds the [manifest](src/domain/manifest.h) before the build writes a generated file. `manifest_builder_check_output_dir` checks `output_dir` against the input roots before the build walks an input tree. `manifest_builder_populate` records every album page, derivative, original, aggregate, and static file output, and rejects the conflicts that the manifest phase lists. The root and input checks compare device and inode values, so they detect alternate spellings, hard links, symlinks, and case-insensitive paths to the same file or directory.
- [output_path](src/build/output_path.h): Reports a generated output path that exceeds either output-path limit, with the same wording for every producer, and builds the `../` prefix from an output file back to the output root. `album_scanner`, `manifest_builder`, and `site_writer` share it.
- [output_pruner](src/build/output_pruner.h): `output_pruner_prune` removes every directory entry below `output_dir` that the manifest does not name and that no output is below, as the prune phase describes.
- [output_writer](src/build/output_writer.h): Makes every change that a build makes under `output_dir`: the writes, the creation of `output_dir`, and the prune's removals. Each write or keep marks its path in the manifest. The writer fails on a path that the manifest does not name, because the same build's prune would remove that path. In a dry run, the writer makes no change and records the removals, and `output_writer_report` lists its marks and removals. The derivative renderer, the site writer, and the prune use the writer for their changes, and use `struct Os` for their reads.
- [page_renderer](src/build/page_renderer.h): The third parallel phase. `page_renderer_render_pages` renders each album into the result slot for its index, in the perm of the worker that renders it. Each render reads the complete album set through a template context that is `const` at every level.
- [site_writer](src/build/site_writer.h): Writes the generated gallery from the rendered results. It writes each album page first. Then it renders and writes the aggregate templates, and copies the discovered static files. It stops at the first output that fails to render, write, or copy.
- [template](src/build/template.h): `template_render_file` uses mustache4c to render a template with a `TemplateContext`. Template names must be safe relative paths, and partial names use a restricted identifier grammar. Each render has three limits. The partial-expansion limit stops a partial that includes itself, so every render ends. The output-byte limit sets the maximum size of the output buffer. The distinct-partial limit sets the maximum size of the compiled-partial cache. The limits do not bound a render's total memory, because templates are trusted input, and a template's nested sections use memory in proportion to the iterations that they request. The module escapes all interpolation for HTML, and rejects unescaped Mustache interpolation. A media node looks up the configured derivative names when it renders, so a new `xl` definition gives `xl.url`, `xl.width`, and `xl.height` without a change to the C model.

### Domain (`src/domain`)

- [album](src/domain/album.h): Defines `Album`, one generated page and its immediate contents. The album, its strings, its media records, and its pointer arrays live in the arena of the skeleton pass. The album records its raw source directory, generated slug path, parent, immediate children and media, recursive cover and item count, output path, URL, depth, and relative path to the output root.
- [build_cache](src/domain/build_cache.h): Parses and formats the build cache. The first line names the generator. Each later line is a row for one source, with tab-separated fields: its relative path, size, modification time, dimensions, orientation and duration. The rows are in order of path. A path that contains a tab or a newline gets no row, so no path needs escaping. `BUILD_CACHE_VERSION` names the row format in the file name.
- [gallery_config](src/domain/gallery_config.h): Defines `GalleryConfig`, `GalleryDerivative`, and the config load and print operations. The load validates and normalizes each value, and copies it into the arena that the caller gives. It rejects a key that is not in the schema, at the top level or in a derivative or `sort` table, and applies the defaults of optional keys. Later phases use the resolved config and do not change it. `base_url` needs an `http://` or `https://` scheme, which the load compares without regard to case, and a host that is not empty. The load removes trailing `/` characters from `base_url` and from each configured directory. It also validates template names, the ordered table of named derivatives, and the `sort` table, and builds the derivative recipe.
- [manifest](src/domain/manifest.h): An insertion-ordered map from `output_path` to `source_label`. The build makes it before it renders a page template or writes an output file. A 4-ary hash trie, with the ASCII-folded path as its key, lets `manifest_add` reject a duplicate. Each trie level uses the next two bits of the path's 64-bit FNV-1a hash, so a lookup goes down O(log n) levels in expectation, and the trie never moves a manifest entry that it placed. Two paths that differ only in ASCII case are duplicates. The fold changes only `A-Z` to `a-z`, and all other bytes, including non-ASCII bytes, must match exactly. `manifest_find_prefix_collision` finds a path that is a `/`-delimited prefix of another path, such as `a/b` and `a/b/c`, or `A/b` and `a/b/c`. Each manifest entry also holds a mark, which is pending until the output writer marks the path as written or kept. A mark changes no trie link, so derivative jobs mark their own paths at the same time.
- [media_item](src/domain/media_item.h): Defines `MediaItem`, one JPEG or MP4 source, and `MediaDerivative`, one configured output for that item. The serial skeleton pass fills the source, derivative-array, original, and URL fields. The probe phase fills only the scalar duration, orientation, and dimension fields. The media derivative array stays aligned with `GalleryConfig.derivatives`. `media_item_has_extension` and `media_item_is_video_path` are the one extension test that the skeleton pass and the video-tool check share to classify a source. The test folds ASCII case.

### Formats (`src/formats`)

- [exif](src/formats/exif.h): Reads the IFD0 orientation from JPEG EXIF data. When the metadata is missing or malformed, the reader uses the top-left orientation.
- [html](src/formats/html.h): HTML escaping of untrusted text for generated markup.
- [image](src/formats/image.h): Buffer-to-buffer JPEG probing, RGB decoding scaled in the DCT, EXIF-orientation transforms, linear-light resizing, crop views that copy no pixels, WebP encoding, and aspect-ratio calculations. The module wraps libjpeg-turbo for probing and decoding, stb for resizing, and libwebp for encoding, and does no filesystem I/O.
- [toml](src/formats/toml.h): TOML text validation at the boundary where parsed bytes become owned text. `toml_require_known_keys` is the shared check for unknown keys. It reports each table key that is not in the list of known keys that a loader gives.

### Runtime (`src/runtime`)

- [fs](src/runtime/fs.h): The POSIX implementation of `os.h`, in an object library of its own, `fram_os_posix`. The `fram` executable and every unit test that is registered without `OS memory` link it, as [BUILD.md](BUILD.md#unit-tests) lists. Its `struct Os` has no state.
- [memory](src/runtime/memory.h): Holds what the out-of-memory report prints. This is the command's log, which `memory_oom_set_log` names, and, for each thread, the file that the thread works on and its job's log, which `memory_oom_set_job` names.
- [os](src/runtime/os.h): The host interface. Every read, write, directory creation, removal, listing, `stat` and print that the program does goes through `os_read`, `os_write`, `os_mkdirs`, `os_remove`, `os_remove_dir`, `os_list`, `os_stat` and `os_print`. Each function that accesses the host takes a `struct Os*`.
  - `os_read` takes a size limit from each caller, and checks it before any read or allocation. It rejects a file that is not a regular file, and a file that changes size during the read. It accepts any bytes, because it also reads media.
  - `os_write` writes either bytes or the contents of a file that it streams, and it creates parent directories. It overwrites its destination in place, through a symlink or a hard link, and the write is neither atomic nor synced, as [The `os` interface](#the-os-interface) explains. A new file gets mode `0666` reduced by the process umask.
  - `os_stat` reports identity, type, size, and modification time.
  - `os_list` closes a directory before it returns. It classifies each directory entry, and for a symlink it classifies the target. It skips dangling links, and directory entries that were removed after it read them.
  - `os_print` carries every byte that the program prints: the log at the end of a command, a rejected command line, `--help` and `--version`, the resolved config, the verbose status lines, and the workers' progress lines, so a test on the in-memory host sees all of it. One call writes its text as a whole. A caller that prints from more than one thread holds its own lock around the call.
  - `os_oom` is the one other member. `sharedstuff` declares it, and each program defines it.
- [pool](src/runtime/pool.h): Runs indexed jobs through a pthread pool of bounded size. It completes every job, even when one job fails, and joins every worker before it returns. Each job gets the index of the worker that runs it, so the job can use that worker's arenas. The module also gets the default worker count from the number of online CPUs. The build runs its jobs in parallel because decoding a JPEG, resizing and encoding a WebP, and running `ffmpeg` take time for each file.
- [proc](src/runtime/proc.h): Runs one argument vector without a shell, and captures the process output up to a limit. It reads standard output and standard error together, so the child never waits on a full pipe. When the child exits with a failure, the reason ends with the newest standard-error bytes that fit, because the last line of the child usually names the cause. Each run of line breaks shows as `; `, so the error stays on one line. The copy of the argument vector that the spawn takes lives in the caller's scratch arena.
- [video](src/runtime/video.h): Wraps `ffprobe` to probe the duration, and `ffmpeg` to extract an auto-rotated JPEG frame. `video_frame_position_ms` places the frame for both the probe and a deferred extraction. Each video has a frame slot: a temporary path that the build allocates before the probe, which `video_extract_frame` fills with `mkostemp`, and a flag that marks a frame as created until `video_frame_remove` removes it.
- [walk](src/runtime/walk.h): Finds and sorts the matching regular files in a directory tree. It uses only `os_list` and `os_stat`, so it behaves the same on every host. It follows symlinked directories, but walks each directory once, by device and inode. It skips a cycle or a second alias of a directory, so an aliased directory publishes one copy of its media. A caller can name one directory to exclude. The walk records that directory as already visited, so it skips every path to it. The build excludes `output_dir`. The walk visits directory entries in byte order, and visits each symlinked directory only after every real directory, so it lists the path without a symlink when such a path exists. Only one directory is open at a time, and no level of the recursion holds a path buffer in its frame.

### Core (`src/core`)

- [ascii](src/core/ascii.h): Locale-independent ASCII byte classification.
- [error](src/core/error.h): Turns an `errno` into text, and is thread-safe. Diagnostics themselves go to the vendored `shared/log.h`, as `printf` formats that the compiler checks.
- [fram_version](src/core/fram_version.h): The tool name and version, which CMake sets when it configures `fram`.
- [parse](src/core/parse.h): Parsing of text into scalar values.
- [path](src/core/path.h): Path construction and validation. `path_is_below` finds the part of a path below a root by text comparison. `manifest_builder` uses it to plan each static file output, and `site_writer` copies the static file to the same path below `output_dir`.
- [path_list](src/core/path_list.h): A growable list of path strings.
- [text](src/core/text.h): Text normalization and validation.

## Cross-cutting conventions

Every module follows these rules. When a design decision is behind a rule, the link goes to its reasons and costs.

### Memory

- Every first-party allocation comes from an arena, except decoded pixels, and each thread has a perm and a scratch. The build's log has an arena of its own. `template_render_file`, `image_encode_webp`, and `os_read` return their result in the caller's perm. See [Memory: arenas only](#memory-arenas-only) and [Memory: a perm and a scratch for each worker](#memory-a-perm-and-a-scratch-for-each-worker).
- A command reserves its memory once, when it starts. It splits the block into arenas of fixed sizes, and the input and render limits bound those sizes. An arena commits pages only when its allocations get to them, and then commits one mebibyte more than the allocation needs. Only committed pages count against the system's memory. See [Memory: reserve, then commit on demand](#memory-reserve-then-commit-on-demand).
- A `struct Image` owns its pixels on `malloc`, and `image_free` is the only place where a caller frees memory. The decode budget bounds the pixels. See [Decoded pixels stay on `malloc`](#decoded-pixels-stay-on-malloc).
- An allocation that does not fit calls `os_oom`, which reports and exits, so no call site checks for allocation failure. The vendored libraries that still use `malloc`, and the pixel allocations, also end in `os_oom`. See [Out of memory exits the process](#out-of-memory-exits-the-process).
- Under the address sanitizer, two arena allocations are never adjacent, so code that extends the last allocation in place uses `arena_extend`. See [Address sanitizer coverage in arenas](#address-sanitizer-coverage-in-arenas).
- A log's arena is never an arena that a callee gets by value, so the log never grows into bytes that a callee uses. See [A log must not grow in a borrowed arena](#a-log-must-not-grow-in-a-borrowed-arena).

### Threads and ownership

Two jobs that run at the same time never share an arena or a log. Each job uses the arenas of the worker that runs it, and each worker has its own arenas. A page job renders into its worker's perm, and every other job allocates only in a copy of its worker's scratch. Before the phases that use them, the serial skeleton pass and the main thread make every allocation in the main thread's perm, including the frame slots. A worker gets immutable shared context, except for the manifest marks of the outputs that it writes or keeps, and it owns one index in each mutable result array. `pool_run` joins every worker before the main thread reads the result slots, and the rendered pages stay in the worker arenas until the write phase. See [Memory: a perm and a scratch for each worker](#memory-a-perm-and-a-scratch-for-each-worker).

### Strings and text input

Every first-party string is a counted `struct Str` with a signed `ptrdiff_t` length, and JPEG bytes travel as one too. Every length, count, and pixel dimension is a `ptrdiff_t`. See [Counted strings with signed lengths](#counted-strings-with-signed-lengths).

`os_read` accepts any bytes, because JPEGs contain `NUL` bytes. No text format gives a meaning to a `NUL` byte, so the text consumers stop a `NUL` before it gets into the generated output. The TOML and template readers reject an embedded `NUL`. `toml_datum_is_text` rejects a TOML string that contains a decoded `NUL`. `toml_require_known_keys` rejects a TOML key that contains a `NUL`, before it names the key. A path that contains a `NUL` fails at the `os` boundary with `EINVAL`, because a system call would read the path only up to that byte.

### Errors and diagnostics

- A fallible action whose caller needs a message beyond success or failure takes a final `struct Log* log`. It records every error that it finds in that log, and never truncates one. A message is one `printf` format that the compiler checks. See [A growable log, and every error in one run](#a-growable-log-and-every-error-in-one-run) and [Log messages use checked format strings](#log-messages-use-checked-format-strings).
- A filesystem, image, process, or video action also takes the `operation` of the caller, such as `failed to read config`, and logs the whole message, because only the action knows which path failed. An image or process action also takes the file that the bytes or the child belong to, and names that file at the end. A failure inside a file names the file as `(in '<file>')` through `log_error_in`.
- An action, and a producer that can fail, return `0` or `-1`, and the producer writes its result through an `_out` parameter. A producer that cannot fail returns its result directly, for example a `struct Str` by value. A predicate returns `bool`. Running out of memory is never a return value. [STYLE.md](STYLE.md#function-signatures) gives the full rules.
- A first-party diagnostic is lowercase prose. It names the failed operation first, puts literal names and values in single quotes, and puts a value after the cause. Messages from the system or from a vendored library use their original capitalization and punctuation. [STYLE.md](STYLE.md#errors-and-diagnostics) gives the full rules.

### Paths and limits

- **Path safety.** Template names must be safe relative paths, and partial names use a more restrictive identifier grammar. Media source paths retain their filesystem bytes, but generated album segments and media filenames are lowercase ASCII slugs. A URL relative to the output root is the output path without change, because every byte of a slug is unreserved in a URL, and templates put the `path_to_root` of the current page before it. The build checks configured output names before it joins them to `output_dir`. These text checks prevent `.` and `..` traversal. Three checks cover symlink aliases that already exist, each at its own boundary: the identity checks of the input roots, the exclusion of `output_dir` from the walks, and the identity checks of the inputs.
- **Input size limits.** Each caller of `os_read` gives a limit for the kind of file that it reads. The read checks the limit before it allocates, so it never loads a file that is too large. `album_scanner` and `derivative_renderer` read media with `IMAGE_INPUT_LEN_MAX` from `image`. This limit is `INT_MAX`, far more than a JPEG within the pixel limit needs, and a read of that size fits in the worker's scratch. `CONFIG_FILE_LEN_MAX` in `gallery_config` is 1 MiB. `TEMPLATE_FILE_LEN_MAX` in `template` is 4 MiB for each template and each partial. `os_write` streams an original from the file itself, so an original has no size limit.
- **Path limits.** The build checks each generated output path against two limits. `OUTPUT_PATH_RELATIVE_LEN_MAX` limits the whole relative path, and `FILENAME_LEN_MAX` limits each path segment. `output_path_check_limits` reports a failure of either limit in the same way for every producer.

### Host access

No code in `src/` outside `runtime/fs.c` makes a system call on a path or prints through `stdio`, except the video code. `proc` and `video` start `ffprobe` and `ffmpeg`, and `video` creates, inspects, and removes the temporary frame that `ffmpeg` writes. They do this on the real host, whichever host an executable links. All other host access goes through `os.h`, and the link step selects the host for each executable. A new unit test links the in-memory host, unless it needs a fact that only a real filesystem has, or it starts a process. See [The `os` interface](#the-os-interface).

## Design decisions

Each decision below states what the code does and why, and some decisions also state what they cost. Read the decision before you change one of these designs, and make sure that the change deals with each of its reasons. The decisions come in groups, in this order: memory, strings, logs and their messages, host access, tests, and where the shared code lives.

### Memory: arenas only

**Decision.** All first-party memory comes from arenas, and the code never frees an individual allocation. Decoded pixels are the one exception, with a decision of their own below. A scratch goes by value, so its memory is free again when the function returns. A perm goes by pointer.

**Reasons.**

- No comment needs to say which code frees a buffer. Only a `struct Image` documents its release.
- The bounds check is one comparison, and the comparison cannot overflow.
- A `Buf` can grow in place when it is the last allocation.
- An arena is one region, not a list of chunks, so a callee that allocates in a by-value copy leaks nothing.

### Memory: a perm and a scratch for each worker

**Decision.** Each pool worker has its own perm and its own scratch, and a job uses the arenas of the worker that runs it. A page job renders its page into a copy of the perm, and stores the copy back as the perm only when the render succeeds. Every other job allocates only in a copy of its worker's scratch. The serial skeleton pass allocates the albums, the media records, and the frame slots in the main thread's perm before the parallel phases start. The build's log has an arena of its own, and `job_run` splits the main thread's scratch into one share of job logs for each worker.

**Reasons.**

- A bump arena is not thread-safe, and separate arenas need no lock.
- A failed render leaves nothing in the arena.
- A job reads its source or frame with `IMAGE_INPUT_LEN_MAX` as the size limit, and a read of that size fits in the worker's scratch, so if a source grew past the limit after the walk, only that job fails.
- Arenas belong to threads, not to albums or media items. A gallery has thousands of media items, and a cap large enough for the largest item, once for each item, would need too much address space.

### Memory: reserve, then commit on demand

**Decision.**

1. When the build starts, it reserves its block with `PROT_NONE`, and it releases the block when it ends. The reservation costs no memory.
2. An arena commits more pages only when an allocation does not fit in the committed part, so the fast path is still one bounds check. The slow path commits pages to one mebibyte past what the allocation needs, but not past the end of the region.
3. Each arena has a fixed size, which comes from the limits that bound what it holds: `IMAGE_INPUT_LEN_MAX`, `IMAGE_ENCODED_LEN_MAX`, the template and render limits, and the expansion limit. Every thread's perm is `BUILD_PERM_LEN` (4 GiB), and every thread's scratch is `BUILD_SCRATCH_LEN` (3 GiB). The log is `BUILD_LOG_LEN` (256 MiB). The main thread's scratch also holds one more `BUILD_LOG_LEN` share of job logs for each worker, and one for `job_run`. `cmd_build.c` defines each size with the bound that justifies it. The worker count is the only input that the reservation depends on.
4. The build runs the number of workers that `-w` requests, or by default one worker for each online CPU. It never runs more than `BUILD_WORKER_PER_CPU_MAX` (four) workers for each online CPU, whatever number `-w` requests. It reserves arenas for that number of workers before it reads any input. `pool_run` starts no more threads than a phase has jobs, so `-w` is a maximum. The verbose status lines give the worker count that the build has arenas for.
5. If the system refuses the block, the build halves its worker count, down to one worker. If the system still refuses, `arena_reserve_halving` halves the block, and every arena with it, up to `ARENA_RESERVE_HALVING_COUNT_MAX` (six) times. It then halves the first size that fits one more time, so that `malloc` in the vendored libraries, decoded pixels, and the thread stacks have room. If the system refuses even the smallest size, the build ends with the out-of-memory report.
6. A worker's arenas live longer than its jobs. Where a `malloc` design would free a buffer that the worker no longer needs, `arena_discard` gives back the buffer's pages instead:
   - a source of `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) or more, after it is probed, and again after it is decoded
   - the bytes of a template, after the template is compiled

**Reasons.**

- Each arena's size comes from the limits, not from an estimate of what the inputs need. An estimate needs a budget that the build reads from `RLIMIT_AS`, estimates for each region from the media counts, the path lengths and the largest JPEG, minimum and maximum sizes, and sized refusals. An estimate helps only under `ulimit -v`, and it can be wrong in both directions: a few hundred album pages of a few mebibytes leave one worker without room, a deep album tree grows past its page estimate after the reservation succeeded, and a budget of half the address-space limit roughly doubles the limit that a gallery needs.
- A fixed reservation from the limits holds every input that the limits accept. A build without an address-space limit runs out of memory only when the system runs out of memory.
- An arena commits its pages on demand, not all at once, because strict overcommit charges every committed page against the system's limit, and Linux ignores `MAP_NORESERVE` under strict overcommit.
- On macOS, a reservation costs system time in proportion to its size: about 1 µs per GiB to reserve, and the same to release. With sizes from the limits, this cost is a fraction of a millisecond. A reservation as large as the address space allows would cost tens of milliseconds on every build.
- One reservation at startup, for the capped worker count, puts the whole layout in one function. A gallery with fewer jobs than workers reserves arenas that some workers never use, and this costs only address space.
- A by-value copy cannot tell the original what it committed, so each job's copies commit their pages again, at a cost of about one system call per job. If the copies shared a committed end, they would skip the poisoning that `arena_commit` does under the address sanitizer. The arena would then hand out bytes that a dead copy left unpoisoned. Committing a worker's first step once, before its jobs, would save about 1% of system time, but it would add a new mechanism, so the code does not do it.
- A job does not give back the pages of a source under `JOB_SOURCE_DISCARD_LEN_MIN`, because the worker's next job reads into the same pages, and faulting them in again would cost more than the read. The derivative phase gives back a large source after it decodes the source and reads its orientation. The pixels hold all that the derivatives need, and the worker's scratch lives longer than the job. Without the discard, the pages of the source would stay resident next to the decodes of other workers, and next to the second buffer of decoded size for a rotated photo. `arena_discard` gives back the bytes of a template, because nothing in the render reuses their pages.

**Cost.** Under a `ulimit -v` that is below the full reservation, the build runs with halved arenas, which hold less than the limits allow. A large input then ends in the out-of-memory report, not in a sized refusal. Under such a limit, only correctness counts: the build succeeds or ends with that report, and it never writes wrong bytes. [Decoded pixels stay on `malloc`](#decoded-pixels-stay-on-malloc) gives the cost for a large decode.

### Out of memory exits the process

**Decision.** When an arena cannot commit more pages, it calls `os_oom`. Each program defines `os_oom`, and the `fram` version does these steps:

1. It prints the errors in the command's log and in the thread's job log.
2. It prints `fram: out of memory` and the name of the file that the thread was processing, as `(in '<file>')`. A command names its log with `memory_oom_set_log`, and a job names its file and log with `memory_oom_set_job`.
3. It exits with `EXIT_CODE_FAILURE`.

**Reasons.**

- `fram` is a CLI tool, and it is easy to make its output directory again.
- No call site checks for allocation failure.
- The report names the file that the thread was working on. A user needs that name to find the input that failed.
- A failed pixel allocation ends the build like any other allocation failure, and does not end only one job, so the build has one failure policy and no `NULL` check after a pixel allocation.

**Vendored code.** mustache4c, tomlc17, libjpeg-turbo, and libwebp still use `malloc`. `fram` replaces tomlc17's allocator with one that calls `os_oom` when `realloc` fails. When mustache4c fails and reports no reason, the wrapper treats the failure as out of memory and calls `os_oom`. Both image codecs report a failed allocation as an error, and `image_decode` and `image_encode_webp` change this error into a call to `os_oom`. The hooks of the stb resizer allocate through `image_alloc`, which calls `os_oom` when `malloc` fails.

**Limit.** The exit skips the build's cleanup of temporary video frames, and it does not stop a running `ffmpeg` child. Ctrl-C and `kill` leave them in the same way, because `src/` installs no signal handlers. With a temporary directory for each build, the cleanup would have only one directory to remove.

### Address sanitizer coverage in arenas

**Decision.** When `fram` is compiled with the address sanitizer, an arena over a reserved region poisons the pages that it commits, and `arena_alloc` unpoisons only the bytes that it returns. Such an arena leaves a poisoned gap before each allocation, so two of its allocations are never adjacent, and code that extends the last allocation in place uses `arena_extend`. An arena over memory that the caller owns has no gap.

**Reasons.** ASan cannot see an overflow that stays inside one large mapping. Without the poisoning, the debug preset loses most of its overflow checks.

**Limits.**

- The arena does not poison scratch memory again when a function returns, so ASan does not find a use of scratch memory after the function that owned it returned.
- When a job's copy of its worker's arenas commits, it poisons the free space again, so a job that allocates past the usable part does not read what an earlier job allocated there.
- `arena_release` leaves the shadow memory to the sanitizer's `munmap` interceptor, which clears it, so a later mapping at the same addresses gets no old poison. Unpoisoning the whole region instead would write an eighth of each reservation as resident shadow memory.

### Decoded pixels stay on `malloc`

**Decision.** A `struct Image` owns its pixels on `malloc`: the output of `image_decode`, `image_orient`, and `image_resize_region`. The working memory of libjpeg-turbo, such as its row buffers and the coefficients of a progressive JPEG, is also on `malloc`, and so are the encoder buffers of libwebp. The resizer buffers of stb use `image_alloc` through the hooks in `vendor/stb/stb.c`. `image_free` is the only way for a caller to release a `struct Image`, and `struct Image` is the one type whose memory a caller frees. `image_alloc` calls `os_oom` instead of returning `NULL`. Everything else on the image path is in arenas: the source bytes and the encoded WebP, in the worker's scratch arena, and every path and message.

**Reasons.**

- An arena gets memory back only when its region is released. If the buffers of the decode were in a worker's scratch, the largest decode of each worker would stay committed for the rest of the build. At the measured peaks for a 100-megapixel 4:2:2 photo, that is up to 302 MiB for a baseline JPEG decoded at full size, and 683 MiB for a progressive one, so about 2.9 GiB or 6.7 GiB with 10 workers. A progressive CMYK one peaks at about 1.05 GiB.
- The decode budget would stop working. It counts a reservation as returned when the job calls `image_free`, but the pages would stay committed in the worker's region, so the worker count would bound memory again.
- Pixels are large and short-lived, the code allocates them once for each file, and the decode budget bounds them. `malloc` handles that case well, and a bump arena handles it badly.
- The exception has one type, `struct Image`, and two files, `formats/image.c` and `vendor/stb/stb.c`. [STYLE.md](STYLE.md#memory-and-resources) allows `malloc` and `free` only in those two files, and in the allocator wrapper that `formats/toml.c` gives to tomlc17.
- Pixels do not go in a worker's region with a primitive that decommits it. Under strict overcommit, `madvise(MADV_DONTNEED)` leaves the pages charged, so the code would need `mmap(MAP_FIXED, PROT_NONE)` to return them. A stale by-value copy whose `end` is above the mark would then write into `PROT_NONE` pages and crash, but `arena.h` documents that a copy which makes pages usable again changes nothing. libwebp has no allocator hooks, so its memory could get into an arena only if the code patched its `malloc` calls, and the budget would have to wait for the decommit. All of this would remove only about eight `malloc` and `free` calls from one module. `arena_discard` avoids the hazard: it maps the pages again as readable and writable, so they stay usable, and it applies to a buffer that the caller names, not to the free space of an arena.
- Pixels get no fixed share of the reservation. The largest decode, of a 100-megapixel progressive CMYK JPEG, peaks at about 1.05 GiB, and the budget leaves room for two such decodes, so a fixed share would refuse any build under a `ulimit -v` below about 3 GB, including galleries of 24-megapixel photos.
- The decode budget does not scale with what the arenas leave, because that would change how many decodes run at the same time on a limited system, and it would need a second planner, based on decode memory.

**Cost.** The pixels get what the arenas leave. Under a tight `RLIMIT_AS`, a large decode that would fit without the arenas can end the build with the out-of-memory report, and the report names the decode whose allocation failed, which is not necessarily the large one.

**Revisited** with the scaled decode of libjpeg-turbo, which bounds the pixels of a baseline JPEG by its largest stale derivative and not by `IMAGE_PIXEL_COUNT_MAX`. A 100-megapixel 4:2:2 baseline JPEG decoded at an eighth peaks at 20 MiB. Pixel scratch for each worker, sized from the configuration, is still not reasonable:

- A progressive JPEG decode holds the coefficients at full resolution at every scale. The same photo as a progressive JPEG peaks at 401 MiB at an eighth, and a progressive CMYK one peaks at about 800 MiB.
- A gallery can configure a derivative up to `IMAGE_ENCODED_DIMENSION_MAX`, so some galleries get no reduction at all.
- The scale changes in steps of an eighth, so a decode can be larger than twice its largest derivative by an eighth of the source in each dimension.

Instead, the scaled decode makes the budget count bytes, so a decode reserves what it holds, not the pixel count of its source.

### Counted strings with signed lengths

**Decision.** Every first-party string is a `struct Str` from [str.h](vendor/sharedstuff/include/shared/str.h): a pointer and a `ptrdiff_t` length, for both owned and borrowed text. JPEG bytes also travel as a `struct Str`. Every length, count, and pixel dimension is a `ptrdiff_t`. A value changes to `size_t` only at a libc, POSIX, or vendored call, or in a C buffer that goes to such a call. A pixel dimension changes to `int` only at a call into stb or libwebp, and to the unsigned `JDIMENSION` of libjpeg-turbo only at a call into libjpeg-turbo. Each such change asserts that the value is not negative, unless this is true by construction. A C string exists only where libc, POSIX, or a vendored API needs one. The POSIX host copies each path into a `PATH_MAX` buffer at the system call, with `copy_cstr`, `error_system_message` fills a caller's buffer through `strerror_r`, and `video` builds each `ffprobe` and `ffmpeg` argument with `str_cstr`.

**Reasons.**

- Subtraction cannot wrap to a very large value.
- A `NUL` byte is an ordinary byte, so no code calls `strlen` on owned text.

**Deliberate.** The `NUL` checks in [Strings and text input](#strings-and-text-input) are a product decision, and the string type does not force them. If the product decision changes, you can remove those checks and the tests that pin them.

### A growable log, and every error in one run

**Decision.**

- Each fallible function whose caller needs a message takes `struct Log* log` as its last parameter, and a callback type puts its `void* userdata` after the log. The log holds every error, with no truncation.
- After an error, the build continues while the next phase still has something to check. It stops before its first write if the log holds an error.
- The log records the errors of the phases in phase order, and the errors of one phase in walk order or job order, so the order does not change with the number of workers.
- A parallel phase prints at most `JOB_ERROR_REPORT_COUNT_MAX` (20) distinct job errors, and a count of the others.
- A rejected config value gets its default, so one bad value causes one error. The build accepts a `derivatives` table only when every entry is valid, and a rejected `base_url` becomes empty.

**Reasons.**

- A user sees every error in one run.
- No message buffer has a fixed size, so no message is truncated, and no message must be early enough in the order to survive truncation.

### A log must not grow in a borrowed arena

**Decision.** A `struct Log` and its `Buf` live in an arena that no callee gets by value. The build gives each log an arena of its own, or an arena that all users of the log get by pointer.

**Reason.** A `Buf` grows in place. If a callee gets a copy of the same arena by value, the callee allocates the same bytes that the log grows into. The two then write over each other, with no crash and no ASan report.

[STYLE.md](STYLE.md#memory-and-resources) states the rule. Treat a break of the rule as a bug, not as a style problem.

### Log messages use checked format strings

**Decision.** A log call takes one `printf` format and its arguments. `log_error`, `log_error_in`, `log_error_v`, `buf_printf`, and `buf_vprintf` have `__attribute__((format(printf, ...)))`. A `struct Str` prints with `%.*s` and `STR_FMT`, and an integer prints with the conversion for its type. For example:

```c
log_error(log, "failed to read template: %s ('%.*s')", reason, STR_FMT(template_path));
```

**Reasons.**

- Each message is one sentence in the source, so a reader reads its wording on one line, and `grep` finds it.
- The compiler checks the number and the types of the arguments against the format. `-Wformat=2` is on for first-party code and for `sharedstuff`.
- `%td` prints a `ptrdiff_t` with no cast. Only a string needs a helper, `STR_FMT`.
- Each first-party function that formats a message has the attribute, so its callers get the same check.

**Costs and limits.**

- `STR_FMT` evaluates its argument two times. Give it a variable or another lvalue, and put a computed string in a variable first.
- A string longer than `INT_MAX` bytes causes an assertion failure at run time, because the `%.*s` precision is an `int`.
- The attribute is the only guard on the code that reads the arguments. Each formatting function measures the text with `vsnprintf`, allocates the text, and writes it with a second `vsnprintf`. These functions are `buf_printf` and `buf_vprintf` in `sharedstuff`, which grow the buffer in place, and the first-party functions that format their own message. Only those functions change the measured `int` length to a `ptrdiff_t`.
- A first-party variadic function does not pass its `va_list` on to `log_error_v` or `buf_vprintf`. If it did, the Clang analyzer, version 22, would report a false `va_end` on an uninitialized list when it targets AArch64 Linux, where a `va_list` is a structure. Instead, the function starts its list once for each `vsnprintf` pass, as `buf_printf` does. `record_error` in `cli.c`, `build_verbose` in `cmd_build.c`, `render_fail` in `template.c`, and `test_log_is` in `tests/test_support.c` follow this rule.
- `%.*s` stops at a `NUL` byte. The text readers reject a `NUL` in config and template content, `toml_datum_is_text` and `toml_require_known_keys` reject a `NUL` in TOML strings and keys, and the `os` boundary rejects a `NUL` in paths, so a message gets no `NUL` from text input. No message prints JPEG bytes. The `os` boundary's `EINVAL` message quotes a path that contains a `NUL` only up to that byte, and the contract test for this case shows the result.
- An `off_t` has no conversion of its own. It prints with `%jd` and a cast to `intmax_t`.
- `clang-format` breaks a string literal that is longer than the column limit, so some long messages are in two literals.

### The `os` interface

**Decision.** All host access goes through `src/runtime/os.h`, except in `proc` and `video`. `fs.c` is the POSIX implementation. [tests/os_memory.c](tests/os_memory.c) is an in-memory implementation for unit tests: an in-memory tree with symlinks, hard links, a modification-time counter, injected failures, and captured output. The link step selects one implementation for each executable. A function that accesses the host takes `struct Os*` just before its arenas, and an `os_*` function takes it first. `proc` and `video` run `ffprobe` and `ffmpeg`, and handle the frame that `ffmpeg` writes, on the real host, whichever host an executable links, because a fake would have to emulate the file output of `ffmpeg`. The video tests write their fake tools to the POSIX host.

**Reasons.**

- Tests of the walk, the manifest, the derivatives, the pages, the writes, and the prune need no temporary directories.
- A link-time choice needs no table of function pointers, because no executable needs two hosts.

**Linking.** `fram` and every unit test registered without `OS memory` link `runtime/fs.c`. The tests registered with `OS memory` link `tests/os_memory.c`, as [BUILD.md](BUILD.md#unit-tests) lists.

**Condition.** Each property that a test checks against the in-memory host must have a contract test. `src/runtime/test_os.c` compiles once against each host and runs the same cases, and both executables must pass, so every property that a test relies on in the in-memory host is also true on POSIX. Without the contract tests, the in-memory host can accept what POSIX rejects. For example, an in-memory host could accept `file/` as a file where POSIX returns `ENOTDIR`, and a contract test checks this case.

**Writes.** `os_write` writes either bytes or a file that it streams in 64 KiB pieces. `os_write`, `os_mkdirs`, `os_remove` and `os_remove_dir` are the only operations that change the host. A copy is an `os_write` from a file, and has no operation of its own. `os_write` writes every output in place, through a symlink or a hard link at the destination, and the write is neither atomic nor synced, so an interrupted build can leave a truncated file. The freshness check keeps such a file until its source changes. Because the output directory is reproducible, deleting `output_dir` and building again is a cheaper recovery than a temporary-file protocol. `os_stat` reports the size and modification time that the freshness check reads.

### Seeded random tests in the normal suite

**Decision.** `tests/test_random.c` checks the parsers of untrusted file headers, the EXIF reader and the JPEG probe, on generated inputs. A well-formed EXIF segment reads back the orientation that the test built it with, and a mutated EXIF segment or JPEG header reads without a crash or a read past the input. The test draws its inputs from the generator in `tests/test_support`, a 64-bit linear congruential step. The seed comes from `FRAM_TEST_SEED`. When `FRAM_TEST_SEED` is not set, the seed is fixed (`TEST_RNG_SEED_DEFAULT`), so the test checks the same cases every time. A failing check prints its seed, and a run with `FRAM_TEST_SEED` set to that seed replays the case. A malformed seed makes the test fail, and does not run a different seed.

**Reasons.**

- The pass runs in every test preset, under the sanitizers of that preset, with no fuzzing engine.
- A fixed default makes the suite deterministic, and a printed seed makes a failure from a different seed reproducible.

**Limit.** The pass reaches only the input shapes that its generators list. An EXIF fuzzing harness would search past these shapes, but the tree does not have one yet.

### Where the shared code lives

**Decision.** `sharedstuff` holds the generic parts: the arena with reserve, commit, and poisoning, `Buf`, `Str`, and `Log`. Each program defines `os_oom`, and has its own `os` interface and its own arena layout. The decoded-pixel exception stays out of `sharedstuff`.

**Reason.** These parts do not depend on what a program does.
