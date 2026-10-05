# Build system

`fram` requires CMake 3.25 or later. This document explains the build targets and their relationships.

## Layout

The top-level CMake file controls the build. It sets project policy, finds the thread dependency, loads build settings, and adds each source directory.

| Path | Responsibility |
| --- | --- |
| `src/CMakeLists.txt` | Build the private application library and the `fram` executable |
| `src/*/CMakeLists.txt` | Add sources, header file sets, and local unit tests |
| `tests/CMakeLists.txt` | Add the golden test suite and the shared unit-test support library |
| `vendor/CMakeLists.txt` | Build each vendored dependency |
| `cmake/fram_build_profiles.cmake` | Set warnings, optimization, fortification, and sanitizers |
| `cmake/fram_quality.cmake` | Configure formatting and static analysis |
| `cmake/fram_testing.cmake` | Add local unit tests |
| `cmake/fram_packaging.cmake` | Configure installation and CPack archives |
| `cmake/fram_run_golden.cmake` | Run one golden test |
| `cmake/fram_restyle_graphviz_svg.cmake` | Add light and dark styles to the target graph |
| `cmake/toolchains/` | Configure Zig for Linux `musl` targets |
| `CMakeGraphVizOptions.cmake` | Set filters and layout for the target graph |

## Target graph

`fram_app` is a private static library. It contains all application source files except `main.c`. The `fram` executable and unit tests link to it. `fram_app` links to `fram_vendor_sharedstuff` for the arena and string buffer implementations.

The library is static because it supports one product. It does not provide a public ABI.

Each vendored project has a separate target. Header-only dependencies use interface libraries.

`fram_app` links to `fram_vendor_tomlc17` and `fram_vendor_sharedstuff` as public dependencies because application headers expose their types. The other vendored dependencies are private.

`Threads::Threads` is a private dependency of `fram_app`. CMake adds this link requirement to each executable that links to the static library.

Source directories group code by function. They do not define separate libraries. Each local `CMakeLists.txt` adds files to `fram_app` and links the private vendored dependency that only its code uses. More libraries would add link boundaries without independent APIs.

The pinned stb implementation has its own translation unit. It restricts decoding to JPEG, keeps decoder errors in thread-local storage for concurrent derivative workers, and keeps vendor warnings separate from first-party compilation.

### Generated dependency graph

CMake generates this graph from the `release` preset. The graph shows CMake targets and link relationships. It does not show dependencies between source modules. Graph options remove test-only targets.

![CMake target dependency graph](media/dependencies.svg)

> [!tip]
> Use these commands to update the graph:
>
> ```sh
> cmake --preset release --graphviz=build/release/target-dependencies.dot
> dot -Tsvg build/release/target-dependencies.dot \
>   -o build/release/target-dependencies.raw.svg
> cmake \
>   -DFRAM_GRAPHVIZ_SVG_INPUT=build/release/target-dependencies.raw.svg \
>   -DFRAM_GRAPHVIZ_SVG_OUTPUT=media/dependencies.svg \
>   -P cmake/fram_restyle_graphviz_svg.cmake
> ```

> [!important]
> CMake reads `CMakeGraphVizOptions.cmake` automatically when you use `--graphviz`. Do not include this file from `CMakeLists.txt`.

## Build profiles

The build uses three interface targets:

- `fram_build_project` sets strict first-party warnings and sanitizer options.
- `fram_build_tests` sets the common warning group and sanitizer options.
- `fram_build_vendor` sets optimization and supported sanitizer options for vendored code.

All three targets require C17 or later. A parent project can select a newer standard. It cannot select an older standard.

Presets use `CMAKE_COMPILE_WARNING_AS_ERROR` to treat warnings as errors. The project does not force this setting on a parent build.

Generator expressions select configuration options at build time. The same rules work with single-config and multi-config generators.

`FRAM_SANITIZER` accepts `none`, `address`, or `thread`. The `address` option enables AddressSanitizer and UndefinedBehaviorSanitizer. The `thread` option enables ThreadSanitizer. It adds sanitizer options to standard CMake configurations. It does not create custom build types.

The address-sanitizer profile instruments first-party and vendored code. Vendored code disables the unsupported `null` and `object-size` checks. The pinned stb translation unit also disables the `shift` check for JPEG bit packing and the `bounds` check for its deliberately biased SIMD lookup pointer. First-party sources retain the full sanitizer set.

## Linting and formatting

CMake sets `clang-tidy` and `cppcheck` as properties of first-party targets. Neither tool checks vendored code.

Tests run `cppcheck`. Tests do not run `clang-tidy` because deliberate failure cases cause analyzer errors.

Cross builds do not run either host analyzer. Zig supplies target headers that the host analyzers cannot find when they repeat the compile command.

The `fram_format` and `fram_lint` targets use an explicit list of first-party files. Add each new source or test to its target and this list.

This duplication is deliberate. A configure-time glob can omit a new file until CMake configures the project again.

## Tests

`FRAM_BUILD_TESTING` controls test creation. It is true by default for a top-level build. It is false by default for a child build.

Each source directory adds its local unit tests. The top-level `tests/` directory adds the golden test suite and `fram_test_support`. Each CTest case has a `fram` label. It also has a `unit` or `golden` label.

`fram_test_support` holds the unit-test plumbing that several `test_*.c` files need: a fixture-root creator, a recursive fixture-tree remover, a fixture-file writer, a working-directory switch, standard-stream capture and restore, and a capture-stream reader. Its own unit test is `tests/test_test_support.c`. `test_support.c` defines `TEST_NO_MAIN`, so acutest's `main` and run state stay in each test executable's own translation unit and the link resolves `acutest_check_` and `acutest_abort_` against it. A failure raised inside the support library is reported against `test_support.c` and fails the test that reached it.

A parent that enables `fram` tests must call `enable_testing()` in its top-level `CMakeLists.txt`. CTest starts discovery at the build root. A child call cannot create the root test file. CMake prints this requirement when a child enables tests.

Each unit test links the same `fram_app` as the executable. No test target compiles an application source a second time with different definitions. The production render limit is 256 MiB, and a failure test at this limit uses too much memory. The template test passes a 64 KiB limit to `template_render_file_limited` instead.

The `photos` golden test covers an image-only gallery with an explicit input directory. The `videos` golden test adds MP4 inputs through the default `media/` directory and is created only when both `ffmpeg` and `ffprobe` are available. This keeps the photo-only suite available on systems without FFmpeg.

Each golden test uses a separate scratch tree. It builds the gallery twice and verifies that the second build leaves every generated file unchanged. The CMake script then compares the output inventory with `tests/expected/<name>`, compares non-media files and copied originals byte for byte after substituting the project version for the `@PROJECT_VERSION@` placeholder in expected pages, and verifies that generated derivatives are nonempty. Derivative bytes are not golden because JPEG output can vary across stb versions and SIMD paths; unit tests pin image dimensions and pixels.

The CMake script collects expected and actual files after `fram` runs. These runtime globs are not build inputs. Fixture gallery changes do not require CMake to configure the project again.

## Version

CMake stores the numeric release version once in `project(VERSION)`. It generates one source file that defines `fram_version_string()` and `fram_generator_string()`. A version change rebuilds this source file and relinks its targets.

Release versions use `MAJOR.MINOR.PATCH`. The `project(VERSION)` command accepts only numeric components. A prerelease suffix requires a second version value.

## Install

The install contains the executable and its documentation. Both use the `fram_runtime` component.

The project has no export set or development component because it does not install a library or header. `GNUInstallDirs` selects the install paths.

## Packaging

### Release packages

CPack creates packages for the different targets `fram-<version>-<target>.tar.gz`.

The macOS deployment target is `13.0` by default. A builder can select a newer target through the cache.

Linux `musl` builds use the two Zig toolchain files. These files select `llvm-strip` because a host strip tool cannot read the cross-built ELF files.

### Source package

CPack can also create a source package `fram-<version>-source.tar.gz`. The ignore list keeps CPack's repository and temporary-file exclusions. It also omits build products.
