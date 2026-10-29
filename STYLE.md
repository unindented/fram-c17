# Style guide

This guide describes the target style for code owned by this repository. It is not a description of the current baseline: existing code that differs from this guide is a deviation to report or fix.

`.clang-format` owns mechanical layout, and `cwrap` refills comment prose to the same 100 columns. Do not restate whitespace, wrapping, indentation, brace-placement, or alignment rules here unless they affect meaning.

Vendored code under `vendor/` is out of scope.

## Influences

This guide is an amalgamation of ideas and conventions from the following resources:

- https://tigerstyle.dev/
- https://man.openbsd.org/style
- https://doc.cat-v.org/bell_labs/pikestyle
- https://www.kernel.org/doc/Documentation/process/coding-style.rst

## Simplicity

- Prefer simple code over clever code. Make the common path obvious.
- Keep functions small and single-purpose.
- Keep each function at one level of abstraction. A high-level function should read as a sequence of sibling operations at the same level of detail.
- Avoid dense expressions with hidden side effects.
- Keep structure definitions focused on the data model.
- Encode repetitive policy as data when it makes the program simpler.
- Prefer simple data structures until measured requirements justify more.
- Do not optimize speculatively. Measure first, then optimize the part that dominates.

## Readability

- Sort functions for reader flow. High-level orchestration functions should appear before lower-level mechanics when forward declarations make that practical.
- Keep tightly related helpers near each other. For mutually recursive functions, place the group together with the minimum forward declarations needed.
- In implementation files, use this order: includes, file-local types and constants, required forward declarations, exported functions in header order, then static helpers grouped by the exported or high-level function they support.
- In unit tests, use this order: shared helpers, common successful behavior, variants and edge cases, then rejection or failure cases.

## Headers

- Keep headers self-contained and minimal.
- Name include guards `FRAM_<FILE>_H`, such as `FRAM_CMD_BUILD_H` in `cmd_build.h`.
- Include the module's own header first in `.c` files.
- Prefer forward declarations for pointer-only dependencies where practical.
- Put implementation details in `.c` files. Keep file-local functions and data `static`.
- Expose the smallest interface needed by other modules.

## Names

- Prefer nouns that carry meaning and compose into derived identifiers.
- Do not abbreviate exported or non-trivial names. Reserve short names for tight local scope.
- Do not give one name multiple context-dependent meanings. Disambiguate instead.
- Use big-endian naming: put most significant word first, units and role suffixes last, so related names group together (`latency_ms_max` rather than `max_latency_ms`).
- Suffix a name by its role: `_path` (filesystem path), `_dir` (directory), `_name` (bare relative name), `_url` (URL), `_out` (output parameter).
- Reserve `len` for a byte length and `count` for an element count, and keep the two distinct. Where a raw buffer still meets libc, pair it with its length as `<x>`/`<x>_len`.
- Suffix a named byte-bound constant by what it measures: `_LEN_MAX` for a maximum length in bytes (`OUTPUT_PATH_RELATIVE_LEN_MAX`, `IMAGE_INPUT_LEN_MAX`), and `_SIZE` for a fixed C buffer's total byte count, terminator included, used directly as the array size (`char message[ERROR_SYSTEM_MESSAGE_SIZE]`). Keep the two distinct.
- Name functions so that call sites read as clear statements. Avoid names that hide whether the return value indicates success, failure, or truth.
- Name every exported function `<module>_<verb>`, where the module is the source file's base name: `gallery_config.c` defines `gallery_config_load`, and `walk.c` defines `walk_files_with_suffixes`. A host implementation is the exception: `fs.c` and `tests/os_memory.c` define the `os_` functions `runtime/os.h` declares, and each names the function that returns its host after that host, `os_posix` and `os_memory`.
- The `<module>_<verb>` scheme names the exported surface only. A file-local (`static`) helper takes a short, readable name chosen to read well at its call site and does not carry the module prefix, which is reserved for exported functions. A host implementation names a step helper of an `os_` function after that function, as the step-helper rule below asks, although `os_` is the exported prefix: `fs.c` has `os_read_bytes` and `os_list_classify`, and `tests/os_memory.c` has `os_write_locked`.
- Directories classify modules by responsibility. Do not repeat a directory name in the symbol.
- When a function breaks an operation into steps and delegates each step to a lower-level helper that only it calls, prefix each such helper with the higher-level function's name to make the delegation chain visible (`make_album` calls `make_album_title`). This applies only to step helpers dedicated to a single caller. A generic helper reused by several callers, or a self-contained routine that merely happens to have one caller today, takes a plain readable name instead. The build pipeline is the exception: the phase helpers of `cmd_build_execute` take the names [ARCHITECTURE.md](ARCHITECTURE.md#build-pipeline) gives the phases, such as `load_build_inputs` and `build_album_skeleton`, and `run_build_phases` runs the phases after the first, so the code and the document name each phase the same way.
- Name predicates and boolean fields, variables, and parameters using `is_*`, or `has_*` to describe what they answer, such as `is_verbose` or `has_failed`.
- Use `snake_case` for functions, variables, fields, directories, and file-local helpers. Source files are named after the module or primary type (`gallery_config.c`), and live in the responsibility directory documented in [ARCHITECTURE.md](ARCHITECTURE.md).
- Use `PascalCase` for `struct` and `enum` tags, such as `GalleryConfig`.
- Use `PascalCase` with an `Fn` suffix for callback typedefs, such as `PoolJobFn`.
- Use `UPPER_SNAKE` for `enum` constants, prefixed by the concept they belong to, such as `CLI_COMMAND_BUILD`.
- Use `UPPER_SNAKE` for a file-local constant that names a fixed literal value: a scalar, a fixed string, or a small list of default literals that stands in for a magic value, such as `INPUT_DIR_DEFAULT`, `ALBUM_TEMPLATE_DEFAULT`, and `JPEG_MARKER_SOI`. Define a constant string as a `struct Str` macro, `#define NAME S("...")`, or as a bare string literal, `#define NAME "..."`, when a `printf` format concatenates it, such as `TEMPLATE_RENDER_ATTRIBUTION`, and a constant list of strings as a `static const struct Str NAME[]` of `STR_INIT("...")` initializers, such as `URL_SCHEMES`. Keep `snake_case` for read-only aggregate data that reads as a data structure rather than a named literal, such as a lookup or dispatch table of specs (`cli_commands`) or a singleton struct instance (`renderer`).
- Treat acronyms as words: `CliOptions` in `PascalCase`, `rendered_html` in `snake_case`.
- Avoid camelCase, Hungarian notation, and type-prefix naming.

## Types

- Use `bool` for real boolean state and predicates.
- Avoid transparent `typedef struct` aliases. Prefer named structs directly.
- Reserve typedefs for callback signatures or aliases that intentionally hide representation.
- Represent text and byte ranges as `struct Str` (`shared/str.h`), passed and returned by value. A `NUL` is an ordinary byte inside one. Test code is the exception where its arguments are literals: the fixture APIs in `tests/os_memory.h` and `tests/test_support.h`, the `contract_*` helpers in `src/runtime/test_os.c`, and the helpers of a test file take and return paths, expected text and messages as C strings, so a test writes its tree and its expectations as literals. A fixture whose contents may hold any byte takes them as a `struct Str`: `os_memory_write_bytes`, whose path is a `struct Str` too, `contract_write` and `contract_is_file`. The capture helpers in `tests/test_support.h` fill a caller's terminated `char` buffer, whose `ptrdiff_t` length converts to `size_t` once, at `fread`.
- Make every length, count, index, and offset `ptrdiff_t`. Convert to a C string or to `size_t` only at the call into libc, POSIX, or a vendored API, with one explicit conversion there, such as `copy_cstr` in `runtime/fs.c` or `(size_t)s.len`. Assert the value is non-negative where that does not already hold by construction.
- Order struct fields to make ownership and invariants obvious: put lifecycle-owned backing storage first, keep pointer/count/capacity fields and value/presence-flag pairs adjacent in that order, keep discriminators before their variant payload, and put independent status flags after the data they describe. Do not reorder a struct when its layout is part of an external ABI, on-disk format, or positional initializer contract.
- Zero-initialize with `= {0}`, whatever the type.

## Function signatures

- Choose a function's return type from its output shape, so call sites read the same everywhere:
  - An action that performs I/O, parses, writes, populates, appends, or runs returns `int`: `0` on success and `-1` on failure.
  - A producer that can fail is an action: it returns `int` and writes its result through an `_out` parameter (`image_decode`, `os_read`).
  - A producer that cannot fail returns its result directly: a `struct Str` by value (`path_join`), or a pointer that is never `NULL`. Allocation is not a failure, per [Memory and resources](#memory-and-resources).
  - A predicate returns `bool` and is named `is_` or `has_`.
  - An infallible query returns its value directly, and only when it genuinely cannot fail.
  - A lifecycle function such as `_init` returns `void`.
- Use an output parameter (`_out`) only when the result cannot be folded into the return value: a fallible producer's result, multiple outputs, populating a caller-owned struct, or a scalar whose whole range is valid so it has no spare sentinel.
- Order parameters as subject, inputs, producing context, outputs, then diagnostics: `(subject, inputs..., os, perm, scratch, *_out..., operation, log)`. The subject is the object the function initializes, mutates, or appends to, and comes first. The producing context sits just before the outputs: the `struct Os*` host, then the `struct Arena* perm` that results are allocated from, then the `struct Arena scratch` for temporaries. An `os_*` function's host is its subject and leads instead. In a callback typedef, the `void* userdata` closure comes after `log`. A function that formats a message ends in `const char* format, ...`, because C puts the variable arguments last, and its subject still leads, as in `log_error(log, format, ...)`. The `image_*` actions and `proc_run` end in `(..., operation, file_path, log)`: there `file_path` names the file a message is about, not a path the function acts on, so it sits with the diagnostics after `operation`.
- Take an arena by pointer when the caller keeps what is allocated from it (`perm`), and by value when the function only needs temporaries (`scratch`), so they are gone when it returns without any cleanup. A function that allocates nothing takes no arena. A low-level helper whose one arena receives its result names it `arena`.
- Give an action a final `struct Log* log` when its caller needs a message beyond success or failure. A filesystem, image, process, or video action also takes the `struct Str operation` that leads its message, just before the log, or just before the `file_path` of the tail above.
- Pass a non-trivial struct by pointer, and a scalar, a `struct Str`, and a `scratch` arena by value. Mark `const` every pointer parameter the function does not modify.
- Give a parameter the same name wherever it fills the same role, so a `struct Log*` is `log` and a `struct Buf*` append target is `buffer` everywhere, not a mix of `buffer` and `buf`.

## Memory and resources

- Allocate every first-party object from an arena, with `arena_new(arena, count, type)` so the allocation follows the target type. Nothing is freed piecemeal: a result lives as long as the arena it came from, so document which arena that is rather than who frees it. Where a `malloc` design would free a buffer that a worker is done with while the worker's arena lives on, give its pages back with `arena_discard` when the work that filled them costs more than faulting them in again, as compiling or rendering does and as reading a source of `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) or more does. Keep the pages of a smaller source, which the worker's next job reads into more cheaply than it could fault them in. Decoded pixels are the one exception: a `struct Image` owns its pixels on `malloc`, through `image_alloc` in `formats/image.c`, and `image_free` releases them, because an arena would keep each worker's largest decode committed for the rest of the build (see the decoded pixels decision in [ARCHITECTURE.md](ARCHITECTURE.md#design-decisions)). Outside the tests, no other first-party code calls `malloc` or `free`, apart from the allocator wrapper a vendored library is given, such as tomlc17's in `formats/toml.c`.
- Out of memory exits. An allocation that does not fit calls `os_oom`, which prints what the command's log and the failing job's log hold so far and `fram: out of memory` with the file the thread was working on, and exits. A job names its file with `memory_oom_set_job` when it starts, and a command names its log with `memory_oom_set_log`. Do not check an arena allocation for `NULL`, guard a size against overflow before allocating, or write an error path for running out of memory. Where a vendored library reports an allocation failure as an ordinary error, map it to `os_oom` at its wrapper, so callers see one policy.
- To leave nothing behind on failure, build into a by-value copy of `perm` and write the copy back only on success (`struct Arena perm = arenas->perm; ... arenas->perm = perm;`). A copy that is never written back serves as scratch in the free tail of `perm`.
- Never assume that two allocations are adjacent. Under the address sanitizer an arena over reserved memory leaves a poisoned gap before each one. To extend the last allocation in place, use `arena_extend`, or `Buf`, `arena_grow` and `str_concat`, which use it. Divide a reservation with `arena_split`, and write an arena over memory the caller owns as `(struct Arena){.beg = beg, .end = end}`, so its `limit` stays `NULL`.
- Never grow a `struct Log` or `struct Buf` in an arena that a callee receives by value while the buffer is in use. The callee's copy hands out the same bytes the buffer grows into, and the sanitizers cannot see the overlap inside one reservation. Give a log an arena of its own, or an arena that everything using it reaches by pointer.
- Keep cleanup symmetric for what an arena does not release: a descriptor, a stream, a memory reservation, a decoded `struct Image`, a temporary video frame, or a vendored library's own allocation. When a function owns several, prefer a single cleanup path over repeated partial cleanup blocks.

## Control flow

- State invariants positively and compare in the natural direction (`if (index < length)`), so the invariant-holds branch reads positively.
- Declare locals at the smallest scope. Introduce a variable where it is first used and do not keep it alive past its use.
- Compute or validate a value close to where it is used, to avoid place-of-check to place-of-use gaps.
- Choose the failure-handling shape from whether the function owns a resource that must be released before returning:
  - When the function owns nothing that needs cleanup, return early on the first failure (`if (step(...) != 0) { return -1; }`). Do not introduce a result variable to carry status past its use.
  - When the function owns a resource that must be released on every path, use a single cleanup path: initialize a result variable to the failure value (`int rc = -1;`), `goto cleanup` on each failure, set the success value only once the work is done, and release the resource under the `cleanup` label before returning.
  - When a single owned resource is threaded through a loop, carry the same result variable in the loop guard (`for (...; rc == 0 && ...;)`) so the loop short-circuits and falls through to the one cleanup path, rather than returning from inside the loop.
  - A short-lived local that only deduplicates one immediate check (a fallible call reached through two shapes, checked on the next line) is still an early return, not a status accumulator, and is fine.

## Limits

- Bound every buffer and loop. Prefer an explicit, named constant over a bare literal.
- Document the limit where it is defined. Enforce it with a `_Static_assert` or a runtime check where practical.
- On overflow, fail fast with a diagnostic that names the limit and the offending value, with the value last (`output path for '<source>' exceeds max output path length (1024 bytes) at 1030 bytes: '<path>'`).

## Errors and diagnostics

- Check every I/O, parsing, and formatting error. Allocation does not fail, per [Memory and resources](#memory-and-resources).
- Signal failure the same way across the whole codebase, following the return convention in [Function signatures](#function-signatures): an action returns `-1` and a predicate returns `false`. Where an external library uses a different convention, confine the translation to a single boundary function.
- Record each error in the caller's `struct Log` with `log_error`, or `log_error_in` for a failure located inside a file. Both return `-1`, so an action can `return log_error(log, ...)`. Only a command boundary prints the log.
- Write a message as one whole sentence in a string-literal `printf` format: `log_error(log, "failed to read template: %s ('%.*s')", reason, STR_FMT(template_path))`. Print a `struct Str` with `%.*s` and `STR_FMT`, and pass `STR_FMT` a plain variable or another lvalue, because it evaluates its argument twice. Hoist a computed string, such as a `str_head` result, into a variable first. Print a C string with `%s`, and an integer with the conversion of its own type: `%td` for `ptrdiff_t`, `%d` for `int` and enum constants, `%u` for `unsigned`, `%zu` for `size_t`, and `PRId64` for `int64_t`. Do not cast an integer only to print it. The one exception is a type with no conversion of its own, such as `off_t`, which goes through `%jd` and `(intmax_t)`. Build an `operation` that holds a value with `buf_printf`.
- Give a function that formats a message the `format(printf, n, m)` attribute, so the compiler checks its callers as it checks `log_error`. Never pass a format that is not a string literal.
- A first-party variadic function formats its own message and does not pass its `va_list` on: it measures the text with `vsnprintf`, allocates it, and writes it with a second `vsnprintf`, starting the list again for each pass, as `buf_printf` does. Clang's analyzer, version 22 on AArch64 Linux, reports a false `va_end` finding on a function that passes its list to `log_error_v` or `buf_vprintf`. `record_error` in `cli.c`, `build_verbose` in `cmd_build.c`, `render_fail` in `template.c`, and `test_log_is` in `tests/test_support.c` do this.
- Keep going past an error wherever the next check still has something to check, so one run reports every mistake, and check `log->errors` before anything that cannot be undone, such as the first write. Never store a rejected value: keep its default, so a later check does not report the same mistake again.
- Keep diagnostics lowercase and without trailing periods. A proper noun keeps its capital wherever it falls, including at the start (`JPEG input exceeds max JPEG input length (...)`), per the naming rule in [Comments](#comments).
- Relay a message that comes from the operating system or a vendored library exactly as that source spells it. The text a user reports has to be findable in the source that produced it, which a rewrite defeats.
- Quote literal names and user-visible values with single quotes, including paths, template names, config keys, media names, and environment variables.
- Lead with the failed operation, then the relevant key, then the cause, such as `config key 'derivatives.s.quality' must be an integer` or `unsupported media source: 'x.raw'`. An input path, an output path, a template name, or a raw argument goes last instead, after the cause: name the subject in prose and let the value trail, as in `failed to read config: No such file or directory ('fram.toml')`. A filesystem action is given that leading operation and writes the rest itself, because only it knows which path failed. An image or process action is also given the file its message names, because its cause arrives with bytes or a child, not a path.
- Decide whether to surface an `errno` by the role of the function that saw it, and translate it only through `error_system_message`. An action that failed at the operating-system boundary surfaces the cause, because the user acts on it and nothing else will report it. A rejection that is first-party policy rather than a system failure carries no `errno`, because none occurred (`exists and is not a directory ('x')`). A classifier consumes an `errno` to tell a routine case from an exceptional one and then discards it, as a walk does when it steps past a dangling symlink. A predicate or fact query takes no `log` and leaves the cause in `errno`, as `os_stat` does, because most callers decide the same either way, and the one that must explain the failure formats the message itself.
- The test for the two ambiguous cases is whether the user would act differently knowing the cause, and whether any later operation will tell them. Surface it when the answer is yes and no: a required directory that cannot be resolved needs `ENOENT` told apart from `EACCES`, and a caller that returns immediately gives nothing else the chance. Drop it when a subsequent operation fails anyway with its own system message, because reporting it twice, the first time from the place with less context, is worse than reporting it once. A predicate is therefore never the right tool for producing a diagnostic: pair it with an action that reports, rather than giving the predicate a `log`.
- Distinguish absent required keys from wrong types: use `missing required ... key` for absence and `... key 'name' must be ...` for invalid values.
- Trail a value in one of three shapes, chosen by the value's role, and do not mix them. Use `: '<value>'` for the value that broke the rule, so the message reads as a judgment of it (`output path segment for '<source>' exceeds max filename length (255 bytes) at 260 bytes: '<segment>'`). Use `('<path>')` for the path an operation targeted when the cause came from somewhere else, such as `errno` or a callee (`failed to read template: No such file or directory ('templates/x.html')`). Use `(in '<file>')`, through `log_error_in`, for the file a failure was located inside (`tag name is invalid at line 1, column 1 (in 'partials/card.html')`). Where a value fills some other role, name that role in words rather than reaching for a bare parenthesis: `(while rendering album '<album>')` when the failure is not in the file being named. A value that reads as part of the sentence stays in the sentence (`config key 'title' must be a string`, `output path for 'media/a.jpg' exceeds max output path length (1024 bytes) at 1030 bytes: '<path>'`). These shapes govern only a value appended after the cause.

## Comments

- Prefer clear code over comments. Do not explain how code works. Rewrite the code to be self-evident instead.
- Use `/** ... */` documentation comments for declarations: functions, types, fields, enum values, named constants, and other declarations whose purpose or contract needs explanation. Use `//` comments for implementation notes, including multi-line notes. Syntax communicates the comment's role, not its length or the declaration's visibility.
- Use comments to explain what a block is for, why it exists, or what constraints it must preserve.
- Use comments for invariants, ownership, protocols, non-obvious algorithms, compatibility constraints, and surprising decisions.
- Keep comments accurate and close to the code they explain. Remove stale comments when changing code.
- Comment on the code as it stands, not on how it got there. A comment narrating its own edit history, or arguing with a decision no longer in the tree, gives a reader nothing to act on. State the reason the code holds today. This applies to a documentation comment as much as to an implementation comment. A pointer to planned work is not history, so a comment explaining why something exists ahead of its first use is fine.
- Write comments as prose: full sentences, capitalized, ending with a period. A trailing end-of-line comment may be a lowercase phrase without punctuation.
- Capitalize language and format names wherever they are prose, including inside a diagnostic (`failed to decode JPEG`, `exceeds max WebP quality (100)`). A name is a name, and lowercasing it makes it harder to recognize and to search for. Identifiers, filenames, and a library's own spelling of itself stay lowercase, because there the text is the token rather than the name (`image_decode`, `formats/image.c`, `mustache4c`, `tomlc17`, `.jpg`).
- Quote literal paths, filenames, directory names, template names, and config keys with backticks, the same as code identifiers in Doxygen blocks.

## Function documentation

- Document every function with a Doxygen block comment (`/** ... */`) so a caller never has to read the body to learn what an argument means or what the return value signals.
- Place the block on the function's declaration: in the header for an exported function, on the file-local forward declaration for a static function that has one, or immediately above the definition for a static function or `main` with no separate declaration. Write the contract once. Do not repeat the block on both the declaration and the definition.
- Open the block with an `@brief` line stating what the function does in the third-person present ("Loads the gallery configuration.", "Appends a single byte."). Keep the voice consistent across the codebase.
- Give one `@param` line per parameter, naming what it holds and any contract the caller must honor: the arena a result lives in, units, valid range, and whether `NULL` is accepted. For an `_out` parameter, describe what is written and when.
- Give an `@return` line whenever the function returns a value. Name the success value and every failure sentinel, following the return conventions in [Function signatures](#function-signatures): an action or fallible producer returns `0` or `-1`, a predicate returns `true` or `false`. Omit `@return` only for a function returning `void`.
- Keep the block to the function's contract. A definition may still carry `//` comments that explain implementation, invariants, or surprising decisions, per [Comments](#comments).

## Tests

- Assert the exact expected value whenever the output is deterministic. Reserve a substring check for cases where an exact match would be brittle rather than strict: long prose such as help text, output from a vendored library already covered by the golden test suite, or a string embedding a temporary directory.
- When a substring check is the right tool, make the needle the complete claim. A needle that is one word of a diagnostic, such as `title`, passes for every message mentioning that word, including the message for a different failure.
- Assert the diagnostic of every failure, not just the non-zero return. A bare `!= 0` cannot tell the failure under test from an unrelated one, and keeps passing when a later change moves the failure to another phase.
- Keep the distinctions required by [Errors and diagnostics](#errors-and-diagnostics) visible to the tests: a missing key and a wrong-typed key must fail different assertions, not the same loose one.
- Derive an expected limit from its named constant rather than repeating the literal. Spell out a literal only when the constant is file-local, and note that changing it must update the test.
- Where correct behavior must not depend on an unspecified detail, such as how `qsort` orders elements that compare equal, assert the same result for more than one input order.
- Do not restate golden test coverage in a unit test. Note which golden test provides the end-to-end assertion instead.
- Order tests as described in [Readability](#readability), and comment each with what it establishes rather than a restatement of its name. Each test case carries a `//` comment. Every helper in a `test_*.c` file carries a Doxygen block and follows [Function documentation](#function-documentation).
- Run a new test against the in-memory host (`fram_add_unit_test(<module> OS memory)`) unless it needs a fact only a real filesystem has, such as the umask, a FIFO, permissions, or an interrupted write. A test that starts a child process, such as the `ffprobe` and `ffmpeg` fakes, stays on the POSIX host, because `proc` and `video` work on the real host whichever host is linked. The video cases of a memory-host module are therefore an executable of their own (`test_*_video.c`). Every unit test registered without `OS memory` links the POSIX host: the POSIX layer's own tests (`fs`, `os_posix`), the test-support test and the tests that run processes or decode video (`proc`, `video` and the `*_video` tests), which need the real host, the suites not yet converted to the in-memory host (`cli`, `cli_dispatch`, `cmd_config`, `gallery_config`, `template`), and the tests that reach no host. A test added to one of those suites runs on the host its suite links. A memory-host test builds its tree with the `os_memory_*` helpers, injects failures with `os_memory_fail`, and has no fixture to remove.
- When a memory-host test relies on a property of a host, such as a message, a symlink rule, or what a trailing `/` means, make sure a contract test in `src/runtime/test_os.c` checks that property. That file runs against both hosts, so the in-memory host cannot accept what POSIX rejects. Injected failures are the exception, because the in-memory host makes them by request.
- Check that running out of memory exits through `test_is_oom`, which runs the code in a child process. Never assert an allocation failure as an error return.
- Draw random inputs from the seeded generator in `tests/test_support.h` (`struct TestRng`), so a random test runs in the normal suite and reaches the same inputs on every run. Its seed comes from `FRAM_TEST_SEED`, and is the fixed `TEST_RNG_SEED_DEFAULT` when that is unset. A failing check names its seed in its message, so setting `FRAM_TEST_SEED` to that seed replays the case. A malformed seed fails the test rather than replaying a different one.
- Never `TEST_ASSERT` while a test's fixture exists. Its abort skips the cleanup and leaves the fixture under `/tmp`. Guard each step with `if (!TEST_CHECK(...)) { goto cleanup; }` and remove the fixture at a single `cleanup` label, or collect results, remove the fixture, then check. Inside a fixture's lifetime this replaces the `TEST_ASSERT` guard on a standard-library pointer.
- Outside a fixture's lifetime, guard a pointer from a standard-library allocation (`fopen`, `tmpfile`, `calloc`) with `TEST_ASSERT` followed by an explicit `if (ptr == NULL) return;`. The assert states the precondition and the `if` is what static analysis can follow: `TEST_ASSERT` hides its abort behind a call whose return value `cppcheck` cannot reason about, so without the `if` it reports `nullPointerOutOfResources` on the next use, and a cross-translation-unit variant in whichever `src/` function receives the pointer. The `if` body is unreachable and does not unwind. An aborted test leaves descriptors and allocations to process exit. An arena allocation never returns `NULL` and needs no such guard.
