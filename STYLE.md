# Style guide

This guide describes the target style for the code that this repository owns. It does not describe the current baseline. Existing code that differs from this guide is a deviation to report or fix. The guide does not apply to vendored code under `vendor/`.

`.clang-format` owns mechanical layout, and `cwrap` refills comment prose to the same 100 columns. Do not restate whitespace, wrapping, indentation, brace-placement, or alignment rules here unless they affect meaning.

## Influences

This guide takes its ideas and conventions from these sources:

- [TigerStyle](https://tigerstyle.dev/)
- [OpenBSD style(9)](https://man.openbsd.org/style)
- [Rob Pike, Notes on Programming in C](https://doc.cat-v.org/bell_labs/pikestyle)
- [Linux kernel coding style](https://www.kernel.org/doc/Documentation/process/coding-style.rst)

## Simplicity

- Prefer simple code over clever code. Make the common path obvious.
- Keep each function small, with a single purpose.
- Keep each function at one level of abstraction. A high-level function should read as a sequence of sibling operations at the same level of detail.
- Avoid dense expressions with hidden side effects.
- Keep structure definitions focused on the data model.
- Encode repeated policy as data when that makes the program simpler.
- Prefer simple data structures until measured requirements justify more complex ones.
- Measure first, then optimize the part that costs the most.

## Readability

- Sort functions in the order that a reader needs them. Put high-level orchestration functions before the lower-level functions that do the mechanics, when forward declarations make that practical.
- Keep closely related helpers near each other. Put a group of mutually recursive functions together, with only the forward declarations that the group needs.
- In implementation files, use this order: includes, file-local types and constants, required forward declarations, exported functions in header order, then static helpers grouped by the exported or high-level function they support.
- In unit tests, use this order: shared helpers, common successful behavior, variants and edge cases, then rejection or failure cases.

## Headers

- Keep headers self-contained and minimal.
- Name include guards `FRAM_<FILE>_H`, such as `FRAM_CMD_BUILD_H` in `cmd_build.h`.
- In a `.c` file, include the module's own header first.
- Where practical, prefer a forward declaration for a dependency that the code uses only through a pointer.
- Put implementation details in `.c` files. Keep file-local functions and data `static`.
- Expose the smallest interface that other modules need.

## Names

### Words and suffixes

- Prefer nouns that carry meaning and that combine into derived identifiers.
- Do not abbreviate exported or non-trivial names. Use short names only in a tight local scope.
- Do not give one name meanings that change with the context. Give each meaning its own name instead.
- Use big-endian naming: put the most significant word first and units and role suffixes last, so that related names group together (`latency_ms_max` rather than `max_latency_ms`).
- Give a name a suffix that shows its role: `_path` (filesystem path), `_dir` (directory), `_name` (bare relative name), `_url` (URL), `_out` (output parameter).
- Name a predicate, and a boolean field, variable, or parameter, with `is_*` or `has_*`, so that the name tells what question it answers, such as `is_verbose` or `has_failed`.
- Use `len` only for a length in bytes and `count` only for a number of elements, and do not use one for the other. Where a raw buffer still goes to or comes from libc, pair the buffer with its length as `<x>`/`<x>_len`.
- Give each named byte-bound constant a suffix that tells what it measures, and do not use one of the two suffixes for the other. `_LEN_MAX` is a maximum length in bytes (`OUTPUT_PATH_RELATIVE_LEN_MAX`, `IMAGE_INPUT_LEN_MAX`). `_SIZE` is the total number of bytes in a fixed C buffer, including the terminator, and the code uses it directly as the array size (`char message[ERROR_SYSTEM_MESSAGE_SIZE]`).

### Functions and files

- Name functions so that call sites read as clear statements. Avoid names that hide whether the return value indicates success, failure, or truth.
- Name every exported function `<module>_<verb>`, where the module is the source file's base name: `gallery_config.c` defines `gallery_config_load`, and `walk.c` defines `walk_files_with_suffixes`. A host implementation is the exception. `fs.c` and `tests/os_memory.c` define the `os_` functions that `runtime/os.h` declares. Each of these files names the function that returns its host after that host: `os_posix` and `os_memory`.
- Give a file-local (`static`) helper a short name that reads well at its call site, without the module prefix, because the `<module>_<verb>` scheme names only exported functions. A host implementation names a step helper of an `os_` function after that function, as the step-helper rule below asks, although `os_` is the exported prefix: `fs.c` has `os_read_bytes` and `os_list_classify`, and `tests/os_memory.c` has `os_write_locked`.
- When a function breaks an operation into steps and delegates each step to a lower-level helper that only it calls, prefix each such helper with the name of the higher-level function, so that the delegation chain is visible (`make_album` calls `make_album_title`). This rule applies only to step helpers that exist for a single caller. Give a generic helper reused by several callers, or a self-contained routine that merely happens to have one caller today, a plain readable name instead.
- The build pipeline is the exception to the step-helper rule. The phase helpers of `cmd_build_execute` take the names that [ARCHITECTURE.md](ARCHITECTURE.md#build-pipeline) gives the phases, such as `load_build_inputs` and `build_album_skeleton`, so that the code and the document use the same name for each phase. `run_build_phases` runs the phases after the first one.
- Name a source file after its module or primary type (`gallery_config.c`). Put the file in the directory for its responsibility, as [ARCHITECTURE.md](ARCHITECTURE.md) documents.
- Directories classify modules by responsibility. Do not repeat a directory name in a symbol.

### Case

- Use `snake_case` for functions, variables, fields, directories, file-local helpers, and source file names.
- Use `PascalCase` for `struct` and `enum` tags, such as `GalleryConfig`.
- Use `PascalCase` with an `Fn` suffix for callback typedefs, such as `PoolJobFn`.
- Use `UPPER_SNAKE` for `enum` constants, with a prefix that names the concept they belong to, such as `CLI_COMMAND_BUILD`.
- Use `UPPER_SNAKE` for a file-local constant that names a fixed literal value: a scalar, a fixed string, or a small list of default literals that stands in for a magic value, such as `INPUT_DIR_DEFAULT`, `ALBUM_TEMPLATE_DEFAULT`, and `JPEG_MARKER_SOI`. Define a constant string of this kind as a `struct Str` macro, `#define NAME S("...")`. When a `printf` format concatenates the string, define it as a bare string literal, `#define NAME "..."`, such as `TEMPLATE_RENDER_ATTRIBUTION`. Define a constant list of strings as a `static const struct Str NAME[]` of `STR_INIT("...")` initializers, such as `URL_SCHEMES`. Keep `snake_case` for read-only aggregate data that reads as a data structure and not as a named literal, such as a lookup or dispatch table of specs (`cli_commands`) or a singleton struct instance (`renderer`).
- Treat acronyms as words: `CliOptions` in `PascalCase`, `rendered_html` in `snake_case`.
- Avoid camelCase, Hungarian notation, and type-prefix naming.

## Types

- Use `bool` for real boolean state and predicates.
- Avoid transparent `typedef struct` aliases. Prefer named structs directly.
- Use typedefs only for callback signatures and for aliases that hide a representation on purpose.
- Represent text and byte ranges as `struct Str` (`shared/str.h`), and pass and return a `struct Str` by value. A `NUL` inside a `struct Str` is an ordinary byte.
  - Test code is the exception where its arguments are literals. The fixture APIs in `tests/os_memory.h` and `tests/test_support.h`, the `contract_*` helpers in `src/runtime/test_os.c`, and the helpers of a test file take and return paths, expected text, and messages as C strings, so that a test can write its tree and its expectations as literals.
    - A fixture whose contents can hold any byte takes the contents as a `struct Str`: `os_memory_write_bytes`, whose path is also a `struct Str`, `contract_write` and `contract_is_file`.
    - The capture helpers in `tests/test_support.h` fill a terminated `char` buffer that the caller supplies. That buffer's `ptrdiff_t` length converts to `size_t` one time, at `fread`.
- Make every length, count, index, and offset `ptrdiff_t`. Convert a value to a C string or to `size_t` only at the call into libc, POSIX, or a vendored API, with one explicit conversion there, such as `copy_cstr` in `runtime/fs.c` or `(size_t)s.len`. Assert that the value is non-negative where its construction does not already guarantee that. `argc` stays the `int` that `main` receives and `copt` takes.
- Order struct fields to make ownership and invariants obvious:
  - Put lifecycle-owned backing storage first.
  - Keep pointer/count/capacity fields and value/presence-flag pairs adjacent, in that order.
  - Put a discriminator before its variant payload.
  - Put independent status flags after the data they describe.
- Do not reorder a struct for these rules when its layout is part of an external ABI, an on-disk format, or a positional initializer contract.
- Use `= {0}` to zero-initialize a value of any type.

## Function signatures

- Choose a function's return type from its output shape, so that call sites read the same everywhere:
  - An action that performs I/O, parses, writes, populates, appends, or runs returns `int`: `0` on success and `-1` on failure.
  - A producer that can fail is an action: it returns `int` and writes its result through an `_out` parameter (`image_decode`, `os_read`).
  - A producer that cannot fail returns its result directly: a `struct Str` by value (`path_join`), or a pointer that is never `NULL`. Allocation is not a failure (see [Memory and resources](#memory-and-resources)).
  - A predicate returns `bool` and has a name that starts with `is_` or `has_`.
  - An infallible query returns its value directly, and only when it cannot fail.
  - A lifecycle function such as `_init` returns `void`.
- Use an output parameter (`_out`) only when the return value cannot hold the result. These are the cases:
  - the result of a fallible producer
  - multiple outputs
  - a caller-owned struct that the function populates
  - a scalar whose whole range is valid, so the scalar has no spare value for a sentinel
- Order parameters as subject, inputs, producing context, outputs, then diagnostics: `(subject, inputs..., os, perm, scratch, *_out..., operation, log)`.
  - The subject is the object that the function initializes, mutates, or appends to. The subject comes first.
  - The producing context comes immediately before the outputs: first the `struct Os*` host, then the `struct Arena* perm` that the function allocates results from, then the `struct Arena scratch` for temporaries.
  - The host of an `os_*` function is its subject, and it comes first instead.
  - In a callback typedef, the `void* userdata` closure comes after `log`.
  - A function that formats a message ends with `const char* format, ...`, because C puts the variable arguments last. Its subject still comes first, as in `log_error(log, format, ...)`.
  - The `image_*` actions and `proc_run` end with `(..., operation, file_path, log)`. In these functions, `file_path` names the file that a message is about, not a path that the function acts on, so it goes with the diagnostics, after `operation`.
- Take an arena by pointer when the caller keeps what the function allocates from it (`perm`). Take an arena by value when the function needs only temporaries (`scratch`), so that the temporaries are gone when the function returns, without any cleanup. A function that allocates nothing takes no arena. A low-level helper that has one arena, and that puts its result in that arena, names the arena `arena`.
- Give an action a final `struct Log* log` when its caller needs a message in addition to success or failure. A filesystem, image, process, or video action also takes the `struct Str operation` that starts its message, immediately before the log, or immediately before `file_path` in the tail that the previous rule describes.
- Pass a non-trivial struct by pointer. Pass a scalar, a `struct Str`, and a `scratch` arena by value. Mark `const` every pointer parameter that the function does not modify.
- Give a parameter the same name everywhere it has the same role. For example, a `struct Log*` is `log` everywhere, and a `struct Buf*` append target is `buffer` everywhere, not sometimes `buffer` and sometimes `buf`.

## Memory and resources

- Allocate every first-party object from an arena, with `arena_new(arena, count, type)`, so that the target type sets the size and alignment of the allocation.
- Do not free memory one piece at a time. A result lives as long as the arena that it came from, so document which arena that is, not who frees the result.
- When a worker no longer needs a buffer that a `malloc` design would free, and the worker's arena continues to exist, give the buffer's pages back with `arena_discard` if the work that filled them costs more than faulting them in again. Compiling and rendering cost more, and so does reading a source of `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) or more. Keep the pages of a smaller source, because the worker's next job reads into them more cheaply than it could fault them in.
- Decoded pixels are the one exception. A `struct Image` owns its pixels on `malloc`: `image_alloc` in `formats/image.c` allocates them, and `image_free` releases them. An arena would keep the largest decode of each worker committed for the rest of the build, as [ARCHITECTURE.md](ARCHITECTURE.md#decoded-pixels-stay-on-malloc) explains. Outside the tests, no other first-party code calls `malloc` or `free`, except the allocator wrapper that first-party code gives to a vendored library, such as the wrapper for tomlc17 in `formats/toml.c`.
- When memory runs out, the program exits. An allocation that does not fit calls `os_oom`. `os_oom` prints the current contents of the command's log and of the failing job's log, then `fram: out of memory` with the file that the thread was working on, and exits. A job names its file with `memory_oom_set_job` when it starts. A command names its log with `memory_oom_set_log`.
- Do not check an arena allocation for `NULL`, guard a size against overflow before allocating, or write an error path for running out of memory. Where a vendored library reports an allocation failure as an ordinary error, map that failure to `os_oom` at its wrapper, so that callers see one policy.
- To leave nothing behind on failure, build into a by-value copy of `perm`, and write the copy back only on success (`struct Arena perm = arenas->perm; ... arenas->perm = perm;`). A copy that the code never writes back is a scratch arena over the free tail of `perm`.
- Never assume that two allocations are adjacent. Under the address sanitizer, an arena over reserved memory leaves a poisoned gap before each allocation. To extend the last allocation in place, use `arena_extend`, or use `Buf`, `arena_grow`, or `str_concat`, which use `arena_extend`. Divide a reservation with `arena_split`. Write an arena over memory that the caller owns as `(struct Arena){.beg = beg, .end = end}`, so that its `limit` stays `NULL`.
- Never grow a `struct Log` or `struct Buf` in an arena that a callee receives by value while the buffer is in use. The callee's copy hands out the same bytes that the buffer grows into, and the sanitizers cannot see this overlap inside one reservation. Give a log an arena of its own, or an arena that all code that uses the log reaches by pointer.
- Keep cleanup symmetric for the resources that an arena does not release: a descriptor, a stream, a memory reservation, a decoded `struct Image`, a temporary video frame, or an allocation that a vendored library makes itself. When a function owns several of these resources, prefer a single cleanup path over repeated partial cleanup blocks.

## Control flow

- State invariants positively, and compare in the natural direction (`if (index < length)`), so that the branch where the invariant holds reads positively.
- Declare each local variable in the smallest scope. Declare a variable where the code first uses it, and do not keep it alive after its use.
- Compute or validate a value near the place where the code uses it, so that there is no gap between the place of the check and the place of the use.
- Choose the failure-handling shape from whether the function owns a resource that it must release before it returns:
  - When the function owns nothing that needs cleanup, return early on the first failure (`if (step(...) != 0) { return -1; }`). Do not introduce a result variable to carry the status past its use.
  - When the function owns a resource that it must release on every path, use a single cleanup path:
    1. Initialize a result variable to the failure value (`int rc = -1;`).
    2. On each failure, `goto cleanup`.
    3. Set the success value only when the work is done.
    4. Under the `cleanup` label, release the resource before returning.
  - When the function carries a single owned resource through a loop, put the same result variable in the loop guard (`for (...; rc == 0 && ...;)`). Then the loop stops at the first failure and falls through to the one cleanup path, and does not return from inside the loop.
  - A short-lived local that only removes the duplication of one immediate check (a fallible call that the code reaches through two shapes, checked on the next line) is permitted. It is still an early return, not a status accumulator.

## Limits

- Set a bound on every buffer and every loop. Prefer an explicit, named constant over a bare literal.
- Document each limit where the code defines it. Where practical, enforce the limit with a `_Static_assert` or a runtime check.
- When a value exceeds a limit, fail immediately with a diagnostic that names the limit and the offending value, and put the value last (`output path for '<source>' exceeds max output path length (1024 bytes) at 1030 bytes: '<path>'`).

## Errors and diagnostics

### Reporting

- Check every I/O, parsing, and formatting error. Allocation does not fail (see [Memory and resources](#memory-and-resources)).
- Signal failure the same way in the whole codebase, with the return convention in [Function signatures](#function-signatures): an action returns `-1`, and a predicate returns `false`. Where an external library uses a different convention, keep the translation in a single boundary function.
- Record each error in the caller's `struct Log` with `log_error`. For a failure at a location inside a file, use `log_error_in`. Both functions return `-1`, so an action can `return log_error(log, ...)`. Only a command boundary prints the log.
- After an error, continue wherever the next check still has something to check, so that one run reports every mistake. Check `log->errors` before any step that cannot be undone, such as the first write.
- Never store a rejected value. Keep its default instead, so that a later check does not report the same mistake again.

### Format strings

- Write a message as one whole sentence in a string-literal `printf` format: `log_error(log, "failed to read template: %s ('%.*s')", reason, STR_FMT(template_path))`. Use `buf_printf` to build an `operation` that holds a value.
- Print a `struct Str` with `%.*s` and `STR_FMT`. Give `STR_FMT` a plain variable or another lvalue, because it evaluates its argument twice. Put a computed string, such as a `str_head` result, into a variable before you pass it to `STR_FMT`.
- Print a C string with `%s`. Print an integer with the conversion for its own type: `%td` for `ptrdiff_t`, `%d` for `int` and enum constants, `%u` for `unsigned`, `%zu` for `size_t`, and `PRId64` for `int64_t`. Do not cast an integer only to print it. The one exception is a type that has no conversion of its own, such as `off_t`. Print that type with `%jd` and `(intmax_t)`.
- Give a function that formats a message the `format(printf, n, m)` attribute, so that the compiler checks its callers in the same way as it checks the callers of `log_error`. Never pass a format that is not a string literal.
- A first-party variadic function formats its own message and does not pass its `va_list` to another formatter such as `log_error_v` or `buf_vprintf`. It measures the text with `vsnprintf`, allocates space for the text, and writes the text with a second `vsnprintf`. It starts the list again for each pass. `buf_printf`, `record_error` in `cli.c`, `build_verbose` in `cmd_build.c`, `render_fail` in `template.c`, and `test_log_is` in `tests/test_support.c` do this. The reason is that Clang's analyzer, version 22 on AArch64 Linux, reports a false `va_end` finding on a function that passes its list to `log_error_v` or `buf_vprintf`.

### Wording

- Keep diagnostics lowercase and without trailing periods. A proper noun keeps its capital letter in every position, including at the start, as in `JPEG exceeds max scan count (...)` (see the naming rule in [Comments](#comments)).
- Relay a message that comes from the operating system or a vendored library exactly as that source spells it, so that the text that a user reports can be found in the source that produced it.
- Quote literal names and user-visible values with single quotes, including paths, template names, config keys, media names, and environment variables.
- Lead with the failed operation, then the relevant key, then the cause, such as `config key 'derivatives.s.quality' must be an integer` or `unsupported media source: 'x.raw'`. An input path, an output path, a template name, or a raw argument goes last instead, after the cause. Name the subject in words and put the value at the end, as in `failed to read config: No such file or directory ('fram.toml')`. A filesystem action receives the operation that starts the message and writes the rest of the message itself, because only the action knows which path failed. An image or process action also receives the file that its message names, because the cause of its failure comes from bytes or from a child process, not from a path.
- Distinguish a missing required key from a key with the wrong type. Use `missing required ... key` when the key is missing, and `... key 'name' must be ...` when its value is invalid.
- Give a trailing value one of three shapes. Choose the shape from the role of the value, and do not mix the shapes:
  - Use `: '<value>'` for the value that broke the rule, so that the message reads as a judgment of that value (`output path segment for '<source>' exceeds max filename length (255 bytes) at 260 bytes: '<segment>'`).
  - Use `('<path>')` for the path that an operation targeted, when the cause came from a different source, such as `errno` or a callee (`failed to read template: No such file or directory ('templates/x.html')`).
  - Use `(in '<file>')`, through `log_error_in`, for the file that contains the location of a failure (`tag name is invalid at line 1, column 1 (in 'partials/card.html')`).
- Where a value has a different role, name that role in words, and do not add a bare parenthesis. For example, use `(while rendering album '<album>')` when the failure is not in the file that the message names. A value that reads as part of the sentence stays in the sentence (`config key 'title' must be a string`, `output path for 'media/a.jpg' exceeds max output path length (1024 bytes) at 1030 bytes: '<path>'`). The three trailing shapes apply only to a value that comes after the cause.

### System errors

- Decide whether to surface an `errno` from the role of the function that saw it, and translate an `errno` only through `error_system_message`:
  - An action that failed at the operating-system boundary surfaces the cause, because the user acts on the cause and nothing else will report it.
  - A rejection that comes from first-party policy, and not from a system failure, carries no `errno`, because no system error occurred (`exists and is not a directory ('x')`).
  - A classifier uses an `errno` to tell a routine case from an exceptional case, and then discards the `errno`. A walk does this when it skips a dangling symlink.
  - A predicate or fact query takes no `log` and leaves the cause in `errno`, as `os_stat` does. The reason is that most callers make the same decision for any cause, and the caller that must explain the failure formats the message itself.
- The test for the two ambiguous cases is whether the user would act differently if they knew the cause, and whether any later operation will tell them the cause. Surface the cause when the answers are yes and no. For example, when a required directory cannot be resolved, the user needs to know if the cause is `ENOENT` or `EACCES`, and a caller that returns immediately gives no other operation the chance to tell them. Drop the cause when a later operation fails anyway with its own system message. Reporting the cause twice, the first time from the place with less context, is worse than reporting it once.
- A predicate is therefore never the right tool for producing a diagnostic. Pair the predicate with an action that reports, and do not give the predicate a `log`.

## Comments

- Prefer clear code over comments. Do not explain how code works. Rewrite the code so that it is self-evident instead.
- Use `/** ... */` documentation comments for declarations: functions, types, fields, enum values, named constants, and other declarations whose purpose or contract needs explanation. Use `//` comments for implementation notes, including multi-line notes. The syntax marks the comment's role, not its length or the declaration's visibility.
- Use comments to explain what a block is for, why it exists, or what constraints it must preserve. Use comments for invariants, ownership, protocols, non-obvious algorithms, compatibility constraints, and surprising decisions.
- Keep comments accurate and close to the code they explain. Remove stale comments when you change code.
- Comment on the code as it stands, not on how it got there, in documentation and implementation comments alike. Do not narrate an edit history, and do not argue with a decision that is no longer in the tree, because that gives a reader nothing to act on. State why the code is as it is today. A pointer to planned work is not history, so a comment can explain why something exists before its first use.
- Write comments as prose: full sentences that start with a capital letter and end with a period. A trailing end-of-line comment can be a lowercase phrase without punctuation.
- Capitalize the names of languages and formats wherever they are prose, including inside a diagnostic (`failed to decode JPEG`, `exceeds max WebP quality (100)`), because a lowercase name is harder to recognize and to search for. Identifiers, filenames, and the spelling that a library uses for its own name stay lowercase, because in those places the text is the token and not the name (`image_decode`, `formats/image.c`, `mustache4c`, `tomlc17`, `.jpg`).
- Quote literal paths, filenames, directory names, template names, and config keys with backticks, in the same way as code identifiers in Doxygen blocks.

## Function documentation

- Document every function with a Doxygen block comment (`/** ... */`), so that a caller never has to read the body to learn what an argument means or what the return value signals.
- Put the block on the function's declaration:
  - For an exported function, put the block in the header.
  - For a static function that has a file-local forward declaration, put the block on that declaration.
  - For a static function or `main` with no separate declaration, put the block immediately above the definition.
- Write the contract one time. Do not repeat the block on both the declaration and the definition.
- Start the block with an `@brief` line that states what the function does, in the third-person present tense ("Loads the gallery configuration.", "Appends a single byte."). Use the same voice in the whole codebase.
- Give one `@param` line for each parameter. The line names what the parameter holds and any contract that the caller must obey: the arena that a result lives in, units, the valid range, and whether the function accepts `NULL`. For an `_out` parameter, describe what the function writes and when.
- Give an `@return` line whenever the function returns a value. Name the success value and every failure sentinel, as the return conventions in [Function signatures](#function-signatures) define them: an action or a fallible producer returns `0` or `-1`, and a predicate returns `true` or `false`. Omit `@return` only for a function that returns `void`.
- Keep the block to the function's contract. A definition can still have `//` comments that explain implementation, invariants, or surprising decisions (see [Comments](#comments)).

## Tests

### Assertions

- Assert the exact expected value whenever the output is deterministic. Use a substring check only where an exact match would be brittle and not strict: long prose such as help text, output from a vendored library that the golden test suite already covers, or a string that contains a temporary directory.
- When a substring check is the right tool, make the needle the complete claim. A needle that is one word of a diagnostic, such as `title`, passes for every message that contains that word, including the message for a different failure.
- Assert the diagnostic of every failure, not only the non-zero return. A bare `!= 0` cannot tell the failure under test from an unrelated failure. It also continues to pass when a later change moves the failure to another phase.
- Keep the distinctions that [Errors and diagnostics](#errors-and-diagnostics) requires visible to the tests: a missing key and a key with the wrong type must fail different assertions, not the same loose assertion.
- Derive an expected limit from its named constant, and do not repeat the literal. Spell out a literal only when the constant is file-local, and note that changing it must update the test.
- Where correct behavior must not depend on an unspecified detail, such as how `qsort` orders elements that compare equal, assert the same result for more than one input order.
- Draw random inputs from the seeded generator in `tests/test_support.h` (`struct TestRng`), so that a random test runs in the normal suite and reaches the same inputs on every run. The seed comes from `FRAM_TEST_SEED`. When `FRAM_TEST_SEED` is not set, the seed is the fixed `TEST_RNG_SEED_DEFAULT`. A failing check names its seed in its message, so that a run with `FRAM_TEST_SEED` set to that seed replays the case. A malformed seed makes the test fail, and does not replay a different case.
- Do not restate golden test coverage in a unit test. Note which golden test provides the end-to-end assertion instead.

### Order and comments

- Order tests as [Readability](#readability) describes. Give each test case a `//` comment that states what the test establishes, and not a comment that restates its name.
- Give every helper in a `test_*.c` file a Doxygen block that follows [Function documentation](#function-documentation).

### Hosts

- Run a new test against the in-memory host (`fram_add_unit_test(<module> OS memory)`) unless it needs a fact that only a real filesystem has, such as the umask, a FIFO, permissions, or an interrupted write. A memory-host test builds its tree with the `os_memory_*` helpers, injects failures with `os_memory_fail`, and has no fixture to remove.
- Keep a test that starts a child process, such as the `ffprobe` and `ffmpeg` fakes, on the POSIX host, because `proc` and `video` work on the real host, whichever host the test links. Thus the video cases of a memory-host module are a separate executable (`test_*_video.c`).
- Every unit test that is registered without `OS memory` links the POSIX host. These tests are:
  - The POSIX layer's own tests (`fs`, `os_posix`), the test-support test, and the tests that run processes or decode video (`proc`, `video` and the `*_video` tests), which need the real host.
  - The suites that are not yet converted to the in-memory host (`cli`, `cli_dispatch`, `cmd_config`, `gallery_config`, `template`).
  - The tests that reach no host.
- A test that is added to one of those suites runs on the host that its suite links.
- When a memory-host test relies on a property of a host, such as a message, a symlink rule, or what a trailing `/` means, make sure that a contract test in `src/runtime/test_os.c` checks that property. That file runs against both hosts, so the in-memory host cannot accept what POSIX rejects. Injected failures are the exception, because the in-memory host makes them on request.
- Use `test_is_oom` to check that running out of memory exits. `test_is_oom` runs the code in a child process. Never assert an allocation failure as an error return.

### Fixtures and pointers

- Outside a fixture's lifetime, guard a pointer from a standard-library allocation (`fopen`, `tmpfile`, `calloc`) with `TEST_ASSERT` followed by an explicit `if (ptr == NULL) return;`. An arena allocation never returns `NULL` and needs no such guard.
  - The assert states the precondition, and static analysis can follow the `if`. `TEST_ASSERT` hides its abort behind a call whose return value `cppcheck` cannot analyze. Without the `if`, `cppcheck` reports `nullPointerOutOfResources` at the next use of the pointer, and a cross-translation-unit variant in the `src/` function that receives the pointer.
  - The `if` body is unreachable and does not unwind. An aborted test leaves its descriptors and allocations to process exit.
- Never use `TEST_ASSERT` while a test's fixture exists, not even to guard a standard-library pointer, because its abort skips the cleanup and leaves the fixture under `/tmp`. Use one of these methods instead:
  - Guard each step with `if (!TEST_CHECK(...)) { goto cleanup; }`, and remove the fixture at a single `cleanup` label.
  - Collect the results, remove the fixture, and then check the results.
