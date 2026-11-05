<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>fram</code> /fɹeɪ̯m/</h1>

`fram` is a small static photo and video gallery generator written in C17. It uses Mustache templates to convert a directory tree of JPEG and MP4 files into album pages, with resized WebP images of each photo and video.

## LLM disclosure

> [!warning]
> I used LLMs extensively to create this project, mostly GPT-5.6 Sol and Claude Opus 5.5.

## Installation

Download a prebuilt binary from the [Releases](https://github.com/unindented/fram-c17/releases) page, or [build it from source](#building).

## Usage

### Commands

`fram` has two commands:

- `fram build`: Generate the gallery in `output_dir`. This command has these flags:
  - `-w, --workers N`: Set the maximum number of worker threads for the build. `N` must be a positive integer. The default is the number of CPUs that `fram` detects. The build never runs more workers than the number of media files or the number of albums, whichever is larger, and never more than four workers for each online CPU.
  - `-n, --dry-run`: Report the changes that the build would make, but do not make them. See [Dry run](#dry-run).
  - `--force`: Ignore the [build cache](#build-cache) for this build.
  - `-v, --verbose`: Print build progress to `stderr`.
- `fram config`: Print the resolved project configuration as TOML.

There's also these options:

- `fram --version` (or `-V`): Print the version.
- `fram --help` (or `-h`): Print the list of commands.
- `fram <command> --help`: Print the help for one command.

A command exits with status 0 when it succeeds, 1 when it fails, and 2 when it rejects the command line.

### Project layout

Run `fram` from a directory that has this layout. The example uses the default directory names:

```text
media/
  Album_One/
    photo-1.jpg
    video-1.mp4
  photo-2.jpeg
static/
  robots.txt
  assets/
    gallery.css
templates/
  partials/
    header.html
    footer.html
  album.html
fram.toml
```

Each part of the project has one purpose:

- `media/` holds the media files: the regular files whose names end in `.jpg`, `.jpeg`, or `.mp4`, in any ASCII letter case, in any directory below `media/`. [Albums and media](#albums-and-media) describes how `fram` makes albums from them. `fram` ignores every other file in `media/`, so it does not copy a `.png` image or a text file. Put a file that the gallery must publish as it is in `static/`.
- `static/` holds the files that `fram` copies into `output_dir` without changes, at the same relative path. For example, `static/robots.txt` becomes `output_dir/robots.txt`.
- `templates/` holds the Mustache templates and their partials, as [Templates](#templates) describes.
- `fram.toml` is the config file, as [Configuration](#configuration) describes.

The media directory and the templates directory must exist, and the static directory is optional. The repository and the release archives contain no starter templates or static files, so each gallery supplies its own.

When `fram` searches `media/` and `static/`, it follows symlinks to directories, but it reads each directory only once:

- If more than one path leads to a directory, `fram` uses only one of them: the path that goes through no symlink, or, if every path goes through a symlink, the first of those paths in byte order. For example, if `album` is a symlink to `trips`, the media files publish only under `trips`.
- `fram` skips a symlink that points back into one of its own parent directories.

### Configuration

`fram.toml` must set `title` and `author`:

```toml
title = "Family Photos"
author = "Daniel Perez Alvarez"
```

This table lists every key that the config file can contain. `fram` rejects any other key, including a key inside a derivative table. For example, if the file contains the misspelled key `ouput_dir`, `fram` cannot load the file. It does not ignore the key and use the default value of `output_dir`.

| Key | Default | Meaning |
| --- | --- | --- |
| `title` | Required | The title of the gallery. The root album also uses this title. |
| `author` | Required | The name of the author. |
| `base_url` | `""` | Optional. The absolute URL where you deploy the gallery. If it is set, it must be an `http://` or `https://` URL with a host. It can have a port, a subpath, and a query string. `fram` removes any trailing `/`. If it is not set, a template can read it as an empty section. |
| `output_dir` | `"public"` | The directory that the build writes to. The build controls all of its contents. See [Output directory](#output-directory). |
| `input_dir` | `"media"` | The directory that contains the media files. |
| `templates_dir` | `"templates"` | The directory that contains the templates and partials. |
| `static_dir` | `"static"` | The directory of files that the build copies into `output_dir` without changes. |
| `album_template` | `"album.html"` | The template for each album page. |
| `aggregate_templates` | `[]` | The templates that the build renders once, with the context of the root album. |
| `derivatives` | `s`, `m`, and `l` | The named derivatives. See [Derivatives](#derivatives). |
| `sort` | `{ albums = "asc", media = "asc" }` | The order of the immediate child albums and the direct media items of each album, by source filename: `"asc"` or `"desc"`. |
| `video_frame_seconds` | `1` | The preferred position of the video poster frame, in seconds. A video that is shorter than this uses its midpoint. |

`fram` removes trailing `/` characters from the values of the four directory keys. A directory value must not be empty. It can be an absolute path, or a relative path that contains `..`.

A template name in the configuration must be a safe relative path. It can contain only letters, digits, `_`, `-`, `.`, and `/`. It cannot contain an empty segment, or the segment `.` or `..`. The name of an aggregate template is both its source path below `templates_dir` and its output path below `output_dir`. So a name that contains `/` creates the same subdirectories in `output_dir`. If `aggregate_templates` is an empty array, the build renders no aggregate templates.

### Output directory

> [!caution]
> The first build into a directory removes everything in that directory that the build did not write, including hidden files. Set `output_dir` to a directory that you use for nothing else.

When a build writes an output, it replaces any file that is already at that path. The build compares the metadata of each media file with the metadata of its outputs, and it skips a derivative or a copied original that is current. After a successful build, the build removes every file and directory below `output_dir` that it did not write, so `output_dir` contains only the outputs of the current inputs. For example, the build removes the pages of removed albums, the outputs of deleted media files and removed static files, the derivatives of an earlier configuration, and anything that you added by hand. If a symlink does not lead to a current output, the build removes the symlink itself. It does not change the file or directory that the symlink points to. A failed build removes nothing. If rendering or writing fails before the build completes, some outputs can be new while other outputs stay unchanged.

Before a build searches the media directory, `fram` rejects `output_dir` in these cases:

- `output_dir` is at or below `input_dir`, `templates_dir`, or `static_dir`. `fram` also makes this check when `output_dir` does not exist yet.
- `output_dir` exists and contains one of those directories or the config file, for example with `output_dir = "."`.

Every search of an input directory skips `output_dir`, so a symlink that leads into `output_dir` cannot make the build read generated files as inputs.

Before it renders page templates or writes output files, `fram` rejects an output plan that has one of these conflicts:

- Two outputs have the same path, for example a static file at the same path as an album page, a derivative, or an aggregate. Paths that differ only in ASCII letter case, such as `assets/Logo.png` and `assets/logo.png`, count as the same path, because a case-insensitive filesystem such as the macOS default stores them as one file.
- One output path must be both a file and a directory. This check also ignores ASCII letter case.
- An output is at or below `input_dir`, `templates_dir`, or `static_dir`, for example through a symlink inside `output_dir`. This check also applies when no file exists at that path yet.
- An output overwrites the config file, a media file, a static file, or any file below `templates_dir`, including partials.

These checks compare the directories themselves, not the text of the paths, so they also find a different spelling of the same directory, or a symlink to it. The build creates `output_dir` only after all of these checks pass, so a rejected build creates nothing.

### Dry run

`fram build --dry-run` runs every phase of a build, probes every media file, and renders every page. But it changes nothing in `output_dir`, and it does not create `output_dir`. It also makes no derivatives, and only checks which derivatives are current. For each path that the build would change or keep, it prints one line to `stdout`. Each line has a mark, a tab, and the path. This is a shortened listing:

```
write	public/album/index.html
keep	public/_fram/v2-s-120x120c-q70-m-1200x800-q85-f1000/album/one-jpg-m.webp
keep	public/_fram/originals/album/one-jpg.jpg
remove	public/_fram/v1-s-120x120c-q70-m-1200x800-q85-f1000
remove	public/_fram/v1-s-120x120c-q70-m-1200x800-q85-f1000/album
remove	public/_fram/v1-s-120x120c-q70-m-1200x800-q85-f1000/album/one-jpg-m.webp
```

| Mark | Meaning |
| --- | --- |
| `write` | The build would write this output. |
| `keep` | The build would not change this derivative or copied original, because it is current. |
| `remove` | The build would remove this path, because the current inputs no longer produce it. The path can be a directory. |

The `write` and `keep` lines come first, in the order that the build plans them:

1. The outputs of each album, in album order.
2. Aggregates.
3. Static files.
4. The build cache.

The `remove` lines come after them, in sorted order. To get only the paths for `sort`, `diff`, or `comm`, use `cut -f2-`. A dry run reports a failure in the same way as a build: on `stderr`, with the same exit code. A dry run that fails prints no listing. With `-v`, the progress messages still go to `stderr`, so `stdout` contains only the listing when you send it through a pipe.

A dry run writes nothing, so a build can still fail after a dry run succeeds. A dry run cannot find these failures:

- The build has no permission to write, or the disk is full.
- `fram` cannot read a static file or an original. A dry run does not open the files that it would copy.
- An old file is at a path where a new output needs a directory, or an old directory is at a path where a new output needs a file. For example, the old output is the file `public/blog`, and the new output is `public/blog/index.html`.
- `output_dir` exists but is not a usable directory.
- A source image is damaged after its header. A dry run does not decode images.

### Build cache

A build writes the build cache to `_fram/v1-build-cache.tsv` below `output_dir`. The build cache records what the build read from each media file: its dimensions, its EXIF orientation, and the duration of a video. The next build reuses the row of a media file if the size and modification time of the file did not change. Thus the build does not read an unchanged photo, does not run `ffprobe` for an unchanged video, and does not read the whole source tree after you edit a template. After a new release, or when the build cache is missing or damaged, the build probes every media file again.

Some tools, such as `exiftool -P`, change a file but keep its size and modification time. After such a change, the build cache can keep a row that no longer describes the file. To correct this, run `fram build --force` once. That build probes every media file again and writes a new build cache.

When you publish `output_dir`, you also publish the build cache. It contains the path of each media file relative to `input_dir`, with the raw filename that page titles and URL slugs change. It also contains the size and modification time of each media file, which no other published file contains. If you do not want to publish this information, exclude `_fram/v1-build-cache.tsv` when you deploy.

### Limits

The build rejects an input file that is too large before it reads the file. The error message gives the limit and the file size.

| File | Limit |
| --- | --- |
| JPEG source or video poster frame | 2,147,483,647 bytes, just under 2 GiB |
| Config file | 1 MiB |
| Template or partial | 4 MiB |

A source image can have at most 100 million pixels.

The build reserves its memory once, when it starts, in fixed sizes that come from these limits. Reserved memory that the build does not use takes address space but no memory. Decoded images are not part of the reservation. An address-space limit (`ulimit -v`) can refuse the full reservation. When that happens, the build runs fewer workers, down to one. If the reservation still does not fit, the build makes it smaller until it fits, so the build has less room for its data. The smaller reservation leaves room for the memory that the build does not reserve, such as decoded images. [ARCHITECTURE.md](ARCHITECTURE.md#memory-reserve-then-commit-on-demand) describes this process. Because the build runs fewer workers before it makes its reservation smaller, the address-space limit that a build needs does not increase with `-w`:

- A build needs about 280 MB (`ulimit -v 270000`). With a lower limit, the build stops at startup with `fram: out of memory`.
- The stacks of the worker threads need room beside the reservation: about 24 MB for two workers. If the limit leaves less room than this, the build fails with `cannot start worker thread`.
- A build can need more memory than it reserved or than the limit leaves, for example for a template whose nested sections expand without bound, or for a decode that is too large for the remaining memory. Then the build prints the errors that the command and the failing job logged up to that point, prints `fram: out of memory (in '<file>')`, and exits with status 1.
- Each worker has at least 256 MiB to keep the errors that it reports. A worker that reports about a million one-line errors, or about 290 files that each contain about 16,000 unknown keys, can also make the build end with that out-of-memory report.

## Albums and media

Each directory below `input_dir` becomes a nested album page if it contains media files or if a directory below it is an album. `input_dir` itself becomes `output_dir/index.html`. For each JPEG, the build makes every configured derivative and copies the original. Each derivative is a lossy WebP image. For each MP4, the build makes the same derivatives from an auto-rotated poster frame, and the copied original is the video that the gallery plays. A gallery with MP4 files needs the `ffmpeg` and `ffprobe` executables on `PATH`. A gallery with only images does not need them.

### Albums

`fram` sorts albums and media items by source filename, in the order that `sort.albums` and `sort.media` set. The default order of both is ascending. With these two settings, album directories that have dates in their names can list the newest album first, while the files in each album stay in the order in which you took them. The cover of an album is the first media item that the album lists. If the album has no media items of its own, its cover is the cover of the first child album that it lists.

In the titles of albums and media items, `fram` shows each underscore as a space. The HTML `download` name of a media item uses this title.

### Derivatives

The default derivatives are these:

| Name | Size | Crop | Quality |
| --- | --- | --- | --- |
| `s` | 120 x 120 | Yes | 70 |
| `m` | 520 x 360 | Yes | 80 |
| `l` | Fits within 792 x 594 | No | 85 |

This `derivatives` table sets the same defaults:

```toml
derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, m = { width = 520, height = 360, quality = 80, crop = true }, l = { width = 792, height = 594, quality = 85 } }
```

A `derivatives` table replaces the defaults and does not add to them, so a gallery that adds a derivative such as `xl` must also list each default derivative that it still uses.

A gallery can define from one through six derivatives. A derivative name must be at most 15 bytes long, must start with an ASCII letter, and can contain only ASCII letters, digits, and `_`. Each name becomes a template field and an output directory. Each derivative must set `width` and `height`, each at most 16,383, which define a bounding box, and `quality`, the WebP quality, from 1 through 100. With `crop = true`, `fram` crops the image around its center to the aspect ratio of the bounding box before it resizes the image. The default value of `crop` is `false`. `fram` does not enlarge a media file that is smaller than the bounding box.

`fram` applies the EXIF orientation before it sizes the derivatives, and then applies the crop policy of each derivative separately. It decodes each media file at a smaller scale, in eighths, and uses the smallest scale that still gives each stale derivative of the file at least twice its own size to resize from. So `fram` decodes only a fraction of the pixels of a large photo. The derivative workers share one memory budget of about 2 GiB to decode media files and write derivatives, so this memory does not increase with `--workers`. The budget lets at least two images of the maximum size decode at the same time, and more images of a smaller size. If a derivative has more than about 55 million pixels, each job that writes it runs alone.

### Output paths

The directory segments of generated albums and the filenames of generated media are lowercase slugs that are safe in a URL. `fram` never renames source files or directories.

Derivatives are at `_fram/<recipe>/<album-slug>/<media-slug>-<derivative>.webp`. The media slug includes the source extension, so `foo.jpg` becomes `foo-jpg`. All derivatives of one media item are thus in one album directory. The recipe name includes the name, dimensions, crop policy, and WebP quality of each derivative, the poster-frame position, and the version of the derivative algorithm. Thus a build never reuses derivatives from an older recipe after you change a setting that affects the output. Originals are at `_fram/originals/<album-slug>/<media-slug>.<source-extension>`.

Every URL that `fram` generates is relative and uses the slugs of the output names, so the same output works through `file://`, or on a server below any URL prefix.

## Templates

Templates use [Mustache](https://mustache.github.io/) syntax and are in `templates_dir`. There are two kinds of templates:

| Kind | Configured by | Rendered |
| --- | --- | --- |
| Album | `album_template` | Once for each album, to the `index.html` of the album. The fields, direct media, immediate child albums, and breadcrumbs of the album are available. |
| Aggregate | `aggregate_templates` | Once, with the context of the root album, to the same relative path below `output_dir`. |

`{{value}}` escapes its output for HTML. `fram` rejects triple-brace and ampersand interpolation, so it escapes every scalar that it renders.

Every template can use these variables:

| Variable | Value |
| --- | --- |
| `gallery.title`, `gallery.author`, `gallery.base_url` | The values from the configuration. |
| `album.title`, `album.url`, `album.path_to_root`, `album.item_count`, `album.cover`, `album.has_media`, `album.has_sub_albums`, `album.parent` | The fields of the current album. They are also available at the top level. `album.cover` is a media value, so its named derivatives are available in the same way as for `media`. |
| `sub_albums` | The immediate child albums, each with the album fields. |
| `breadcrumbs` | The albums from the root album through the current album, each with the album fields. |
| `media` | The media items directly in the current album. Each item has `title`, `is_image`, `is_video`, `original_url`, and `duration`, the length of a video in milliseconds. Each configured derivative is also a named child with `url`, `width`, and `height`, so the defaults give `s.url`, `s.width`, `s.height`, `m.url`, and so on. If a derivative has the same name as a built-in media field, the built-in field takes precedence. |
| `generator` | The name and version of `fram`, for a tag such as `<meta name="generator" content="{{generator}}">`. |
| `path_to_root` | The relative prefix from the current output page to `output_dir`. |

A partial reference `{{> name}}` loads `templates_dir/partials/<name>.html`. A partial name can contain only letters, digits, `_`, and `-`.

```html
{{> header}}
<main>
  <h1>{{album.title}}</h1>
  <ul>
    {{#media}}
    <li><a href="{{original_url}}"><img src="{{s.url}}" width="{{s.width}}" height="{{s.height}}" alt="{{title}}"></a></li>
    {{/media}}
  </ul>
</main>
{{> footer}}
```

## Development

[ARCHITECTURE.md](ARCHITECTURE.md) describes the code. [BUILD.md](BUILD.md) describes the CMake targets and the reasons for the configurations below. [STYLE.md](STYLE.md) describes the code style.

### Prerequisites

- A POSIX-like system with CMake 3.25 or later, a C17 compiler such as Clang or GCC, and pthreads.
- NASM on x86-64, for the SIMD routines of libjpeg-turbo.
- `ffmpeg` and `ffprobe`, for galleries with MP4 files and for the golden test.
- Optional build tools: `ninja` for the `multi` preset.
- Optional quality tools: `cwrap`, `clang-format`, `clang-tidy`, `cppcheck`.
- Zig 0.17 and `llvm-strip` for the Linux `musl` archives.

The source tree includes the dependencies in [vendor/](vendor/).

### Building

Each preset builds in `build/<preset>`, outside the source tree, and treats compiler warnings as errors. If `CC` is set, the preset builds in `build/<preset>/<CC>` instead, such as `build/debug/gcc-14`. CMake reads `CC` only when it configures a directory for the first time, so each compiler needs its own CMake cache. The commands use eight parallel jobs. Change that number to suit your machine.

```sh
cmake --preset debug
cmake --build --preset debug -j 8
```

| Preset | Configuration | Binary |
| --- | --- | --- |
| `debug` | `Debug` with AddressSanitizer and UndefinedBehaviorSanitizer. It runs `clang-tidy` and `cppcheck` during compilation if they are available. | `build/debug/bin/fram` |
| `tsan` | `Debug` with ThreadSanitizer. | `build/tsan/bin/fram` |
| `release` | `RelWithDebInfo`, with no tests. The binary in the build tree keeps its debug information, and packaging strips the installed copy. | `build/release/bin/fram` |
| `multi` | `Debug` and `RelWithDebInfo` in one build tree, with the `Ninja Multi-Config` generator. Build with `--preset multi-debug` or `--preset multi-relwithdebinfo`. | `build/multi/bin/Debug/fram`, `build/multi/bin/RelWithDebInfo/fram` |

`clang-tidy` runs only with Clang, because it rejects the warning options of GCC. To run linting, configure the `debug` preset, and then run this command:

```sh
cmake --build --preset lint
```

### Testing

CTest registers the unit tests. It also registers the end-to-end golden test if the configure step finds `ffmpeg` and `ffprobe`. Build a preset, and then run its tests:

```sh
ctest --preset debug -j 8
```

The test presets are `debug`, `tsan`, `multi-debug`, and `multi-relwithdebinfo`. The `multi-relwithdebinfo` tests check the same CMake configuration as the `release` preset, which builds no tests. To run only part of a suite, select the tests by label or by name pattern:

```sh
ctest --preset debug -L unit -j 8
ctest --preset debug -L golden -j 8
ctest --preset debug -R template -j 8
```

### Continuous integration

The workflow presets run the same configure, build, and test steps as CI:

| Workflow | Steps |
| --- | --- |
| `cmake --workflow --preset ci-debug` | Build in `Debug`, run linting, and run all ASan and UBSan tests. |
| `cmake --workflow --preset ci-tsan` | Build with TSan and run all TSan tests. |
| `cmake --workflow --preset ci-release` | Build in `RelWithDebInfo`. |
| `cmake --workflow --preset ci-multi` | Build and test in `Debug` and `RelWithDebInfo` with the `Ninja Multi-Config` generator. CI runs this workflow a second time with `CC=gcc-14` to check GCC. |

To run the Linux workflows on a machine with Podman, build the pinned Ubuntu image from [Containerfile](Containerfile). Then run each workflow in a new container:

```sh
podman build --tag fram-linux-ci --file Containerfile .
podman run --rm fram-linux-ci ci-debug
podman run --rm fram-linux-ci ci-tsan
podman run --rm fram-linux-ci ci-release
podman run --rm fram-linux-ci ci-multi
podman run --rm -e CC=gcc-14 fram-linux-ci ci-multi
```

The image contains a snapshot of the source tree, including the vendored dependencies, without `.git/` and `build/`, which [.containerignore](.containerignore) excludes, so the Linux build products stay in the container and do not go into the `build/` directory of the host. The image supports x86-64 and AArch64 hosts, and pins LLVM 23 and GCC 14. It does not include Zig, because only the release archives need Zig.

### Packaging

Only the release configurations have package presets. Build a preset, and then package it:

```sh
cmake --preset release
cmake --build --preset release -j 8
cpack --preset release
```

| Preset | Archive |
| --- | --- |
| `release` | A stripped native archive in `build/release/`. |
| `release-linux-x86_64` | `build/release-linux-x86_64/fram-<version>-x86_64-linux-musl.tar.gz`, cross-built with Zig. |
| `release-linux-aarch64` | `build/release-linux-aarch64/fram-<version>-aarch64-linux-musl.tar.gz`, cross-built with Zig. |

A source archive needs a configured tree but no compiled binary. It does not contain build products or private workspace metadata:

```sh
cmake --preset release
cpack --config build/release/CPackSourceConfig.cmake
```

The source archive is `build/release/fram-<version>-source.tar.gz`.

The version of every archive comes from `project(VERSION)` in [CMakeLists.txt](CMakeLists.txt). Tag each release commit with the matching `vMAJOR.MINOR.PATCH`.

## Dependencies

[vendor/](vendor/) contains these dependencies. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists their licenses.

- [sharedstuff](https://github.com/unindented/sharedstuff-c17): Arena allocation, counted strings, output buffers, and the diagnostic log.
- [copt](https://github.com/fardaniqbal/copt): Command-line option parsing.
- [tomlc17](https://github.com/cktan/tomlc17): TOML configuration parsing.
- [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo): JPEG decoding.
- [libwebp](https://chromium.googlesource.com/webm/libwebp): WebP encoding.
- [stb_image_resize](https://github.com/nothings/stb_image_resize): Image resizing.
- [mustache4c](https://github.com/mity/mustache4c): Mustache template rendering.
- [acutest](https://github.com/mity/acutest): Unit tests.

## License

`fram` is MIT-licensed, as [LICENSE.txt](LICENSE.txt) states.
