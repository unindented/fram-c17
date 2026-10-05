# Style guide

This guide describes the target style for code owned by this repository. It is not a description of the current baseline: existing code that differs from this guide is a deviation to report or fix.

`.clang-format` owns mechanical layout. Do not restate whitespace, wrapping, indentation, brace-placement, or alignment rules here unless they affect meaning.

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
- Pair a raw byte buffer with its length as `<x>`/`<x>_len`. Reserve `len` for a byte length and `count` for an element count, and keep the two distinct.
- Suffix a named byte-bound constant by what it measures: `_LEN_MAX` for a maximum content length that excludes the terminator (a caller adds room for it, as in `char at_path_limit[OUTPUT_PATH_RELATIVE_LEN_MAX + 1]`), `_SIZE` for a fixed buffer's total byte count that includes the terminator (used directly as the array and `snprintf` size, as in `char err[ERROR_MESSAGE_SIZE]`), and `_CAPACITY_MIN` for the minimum starting capacity of a growable container that tracks fill separately (`STRING_BUFFER_CAPACITY_MIN` seeds the `capacity` field). Keep the three distinct.
- Name functions so that call sites read as clear statements. Avoid names that hide whether the return value indicates success, failure, truth, or ownership transfer.
- Name every exported function `<module>_<verb>`, where the module is the source file's base name: `gallery_config.c` defines `gallery_config_load`, and `fs.c` defines `fs_read_file`.
- The `<module>_<verb>` scheme names the exported surface only. A file-local (`static`) helper takes a short, readable name chosen to read well at its call site and does not carry the module prefix, which is reserved for exported functions.
- Directories classify modules by responsibility. Do not repeat a directory name in the symbol.
- When a function breaks an operation into steps and delegates each step to a lower-level helper that only it calls, prefix each such helper with the higher-level function's name to make the delegation chain visible (`make_album` calls `make_album_title`). This applies only to step helpers dedicated to a single caller. A generic helper reused by several callers, or a self-contained routine that merely happens to have one caller today, takes a plain readable name instead.
- Name predicates and boolean fields, variables, and parameters using `is_*`, or `has_*` to describe what they answer, such as `is_verbose` or `has_failed`.
- Use `snake_case` for functions, variables, fields, directories, and file-local helpers. Source files are named after the module or primary type (`gallery_config.c`), and live in the responsibility directory documented in [ARCHITECTURE.md](ARCHITECTURE.md).
- Use `PascalCase` for `struct` and `enum` tags, such as `GalleryConfig`.
- Use `PascalCase` with an `Fn` suffix for callback typedefs, such as `PoolJobFn`.
- Use `UPPER_SNAKE` for `enum` constants, prefixed by the concept they belong to, such as `CLI_COMMAND_BUILD`.
- Use `UPPER_SNAKE` for a file-local constant that names a fixed literal value: a scalar, a fixed string, or a small list of default literals that stands in for a magic value, such as `INPUT_DIR_DEFAULT`, `ALBUM_TEMPLATE_DEFAULT`, and `JPEG_MARKER_SOI`. Declare a constant string pointer as `static const char* const NAME` so neither the pointer nor the text can be rebound. Keep `snake_case` for read-only aggregate data that reads as a data structure rather than a named literal, such as a lookup or dispatch table of specs (`cli_commands`) or a singleton struct instance (`renderer`).
- Treat acronyms as words: `CliOptions` in `PascalCase`, `rendered_html` in `snake_case`.
- Avoid camelCase, Hungarian notation, and type-prefix naming.

## Types

- Use `bool` for real boolean state and predicates.
- Avoid transparent `typedef struct` aliases. Prefer named structs directly.
- Reserve typedefs for callback signatures or aliases that intentionally hide representation.
- Order struct fields to make ownership and invariants obvious: put lifecycle-owned backing storage first, keep pointer/count/capacity fields and value/presence-flag pairs adjacent in that order, keep discriminators before their variant payload, and put independent status flags after the data they describe. Do not reorder a struct when its layout is part of an external ABI, on-disk format, or positional initializer contract.

## Function signatures

- Choose a function's return type from its output shape, so call sites read the same everywhere:
  - A producer that allocates, creates, duplicates, looks up, or transforms into new storage returns the resulting pointer, or `NULL` on failure. This is the `malloc`/`strdup` convention.
  - An action that performs I/O, parses, writes, populates, appends, or runs returns `int`: `0` on success and `-1` on failure.
  - A predicate returns `bool` and is named `is_`, `has_`, or `valid_`.
  - An infallible query returns its value directly, and only when it genuinely cannot fail.
  - A lifecycle function such as `_init` or `_free` returns `void`.
- Use an output parameter (`_out`) only when the result cannot be folded into the return value: multiple outputs, populating a caller-owned struct, or a scalar whose whole range is valid so it has no spare sentinel.
- Order parameters as subject, inputs, producing context, outputs, then diagnostics: `(subject, inputs..., arena, *_out..., err, err_len)`. The subject is the object the function initializes, mutates, or appends to, and comes first. A producing arena that is not the subject sits just before the outputs. The `err`/`err_len` diagnostic pair is always last.
- Attach a diagnostic buffer (`char* err, size_t err_len`) to an action when the caller needs a message beyond success or failure. Format into it with the shared error helper.
- Pass a non-trivial struct by pointer and a scalar by value. Mark `const` every pointer parameter the function does not modify.
- Give a parameter the same name wherever it fills the same role, so `struct StringBuffer*` append targets are all `buffer`, not a mix of `buffer` and `buf`.

## Resources and ownership

- Make ownership transfer explicit in names, comments, or API contracts, using a consistent vocabulary.
- Keep cleanup symmetric. When a function owns several resources, prefer a single cleanup path over repeated partial cleanup blocks.
- Use `sizeof(*ptr)` allocation style so allocations follow the target type.

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
- On overflow, fail fast with a diagnostic that names the limit and the offending value, with the value last so a long value cannot truncate the limit off the end (`output path exceeds max output path length (1024 bytes) at 1030 bytes (for 'x.jpg'): 'x'`).

## Errors and diagnostics

- Check allocation, I/O, parsing, and formatting errors.
- Signal failure the same way across the whole codebase, following the return convention in [Function signatures](#function-signatures): a producer returns `NULL`, an action returns `-1`, and a predicate returns `false`. Where an external library uses a different convention, confine the translation to a single boundary function.
- Keep diagnostics lowercase and without trailing periods. A proper noun keeps its capital wherever it falls, including at the start (`JPEG input exceeds max JPEG input length (...)`), per the naming rule in [Comments](#comments).
- Relay a message that comes from the operating system or a vendored library exactly as that source spells it. The text a user reports has to be findable in the source that produced it, which a rewrite defeats.
- Quote literal names and user-visible values with single quotes, including paths, template names, config keys, media names, and environment variables.
- Lead with the failed operation, then the relevant key when that value is bounded, then the cause, such as `config key 'derivatives.s.quality' must be an integer` or `unsupported media source: 'x.raw'`. An unbounded value goes last instead, after the cause. That covers an input path, an output path, a template name, and a raw argument, so a pathological value truncates itself rather than the reason it was meant to explain. A filesystem path is never treated as bounded, even when every current caller passes a literal: name the subject in prose and let the path trail, as in `failed to read config: No such file or directory ('fram.toml')`. What a caller happens to pass today is not a bound the message can rely on.
- Decide whether to surface an `errno` by the role of the function that saw it, and translate it only through `error_system_message`. An action that failed at the operating-system boundary surfaces the cause, because the user acts on it and nothing else will report it. A rejection that is first-party policy rather than a system failure carries no `errno`, because none occurred (`exists and is not a directory ('x')`). A classifier consumes an `errno` to tell a routine case from an exceptional one and then discards it, as a walk does when it steps past a dangling symlink. A predicate or fact query takes no `reason` parameter at all and drops the cause, as `fs_identify` does, because the caller's decision is the same either way.
- The test for the two ambiguous cases is whether the user would act differently knowing the cause, and whether any later operation will tell them. Surface it when the answer is yes and no: a required directory that cannot be resolved needs `ENOENT` told apart from `EACCES`, and a caller that returns immediately gives nothing else the chance. Drop it when a subsequent operation fails anyway with its own system message, because reporting it twice, the first time from the place with less context, is worse than reporting it once. A predicate is therefore never the right tool for producing a diagnostic: pair it with an action that reports, rather than widening the predicate.
- Distinguish absent required keys from wrong types: use `missing required ... key` for absence and `... key 'name' must be ...` for invalid values.
- Trail a value in one of three shapes, chosen by the value's role, and do not mix them. Use `: '<value>'` for the value that broke the rule, so the message reads as a judgment of it (`output path exceeds max output path length (1024 bytes) at 1030 bytes (for 'x.jpg'): 'x'`). Use `('<path>')` for the path an operation targeted when the cause came from somewhere else, such as `errno` or a callee (`failed to read template: No such file or directory ('templates/x.html')`). Use `(in '<file>')` for the file a failure was located inside (`tag name is invalid at line 1, column 1 (in 'partials/card.html')`). Where a value fills some other role, name that role in words rather than reaching for a bare parenthesis: `(for '<source>', to '<destination>')` when there are two, and `(while rendering album '<album>')` when the failure is not in the file being named. A value that reads as part of the sentence stays in the sentence (`config key 'title' must be a string`). These shapes govern only a value appended after the cause.

## Comments

- Prefer clear code over comments. Do not explain how code works. Rewrite the code to be self-evident instead.
- Use `/** ... */` documentation comments for declarations: functions, types, fields, enum values, named constants, and other declarations whose purpose or contract needs explanation. Use `//` comments for implementation notes, including multi-line notes. Syntax communicates the comment's role, not its length or the declaration's visibility.
- Use comments to explain what a block is for, why it exists, or what constraints it must preserve.
- Use comments for invariants, ownership, protocols, non-obvious algorithms, compatibility constraints, and surprising decisions.
- Keep comments accurate and close to the code they explain. Remove stale comments when changing code.
- Comment on the code as it stands, not on how it got there. A comment narrating its own edit history, or arguing with a decision no longer in the tree, gives a reader nothing to act on. State the reason the code holds today. This applies to a documentation comment as much as to an implementation comment. A pointer to planned work is not history, so a comment explaining why something exists ahead of its first use is fine.
- Write comments as prose: full sentences, capitalized, ending with a period. A trailing end-of-line comment may be a lowercase phrase without punctuation.
- Capitalize language and format names wherever they are prose, including inside a diagnostic (`failed to decode JPEG`, `JPEG quality must be from 1 through 100`). A name is a name, and lowercasing it makes it harder to recognize and to search for. Identifiers, filenames, and a library's own spelling of itself stay lowercase, because there the text is the token rather than the name (`image_decode`, `formats/image.c`, `mustache4c`, `tomlc17`, `.jpg`).
- Quote literal paths, filenames, directory names, template names, and config keys with backticks, the same as code identifiers in Doxygen blocks.

## Function documentation

- Document every function with a Doxygen block comment (`/** ... */`) so a caller never has to read the body to learn what an argument means or what the return value signals.
- Place the block on the function's declaration: in the header for an exported function, on the file-local forward declaration for a static function that has one, or immediately above the definition for a static function or `main` with no separate declaration. Write the contract once. Do not repeat the block on both the declaration and the definition.
- Open the block with an `@brief` line stating what the function does in the third-person present ("Loads the gallery configuration.", "Appends a single byte."). Keep the voice consistent across the codebase.
- Give one `@param` line per parameter, naming what it holds and any contract the caller must honor: ownership, units, valid range, and whether `NULL` is accepted. For an `_out` parameter, describe what is written and when.
- Give an `@return` line whenever the function returns a value. Name the success value and every failure sentinel, following the return conventions in [Function signatures](#function-signatures): a producer returns the new pointer or `NULL`, an action returns `0` or `-1`, a predicate returns `true` or `false`. Omit `@return` only for a function returning `void`.
- Keep the block to the function's contract. A definition may still carry `//` comments that explain implementation, invariants, or surprising decisions, per [Comments](#comments).

## Tests

- Assert the exact expected value whenever the output is deterministic. Reserve a substring check for cases where an exact match would be brittle rather than strict: long prose such as help text, output from a vendored library already covered by the golden test suite, or a string embedding a temporary directory.
- When a substring check is the right tool, make the needle the complete claim. A needle that is one word of a diagnostic, such as `strstr(err, "title")`, passes for every message mentioning that word, including the message for a different failure.
- Assert the diagnostic of every failure, not just the non-zero return. A bare `!= 0` cannot tell the failure under test from an unrelated one, and keeps passing when a later change moves the failure to another phase.
- Keep the distinctions required by [Errors and diagnostics](#errors-and-diagnostics) visible to the tests: a missing key and a wrong-typed key must fail different assertions, not the same loose one.
- Derive an expected limit from its named constant rather than repeating the literal. Spell out a literal only when the constant is file-local, and note that changing it must update the test.
- Where correct behavior must not depend on an unspecified detail, such as how `qsort` orders elements that compare equal, assert the same result for more than one input order.
- Do not restate golden test coverage in a unit test. Note which golden test provides the end-to-end assertion instead.
- Order tests as described in [Readability](#readability), and comment each with what it establishes rather than a restatement of its name. Each test case carries a `//` comment. Every helper in a `test_*.c` file carries a Doxygen block and follows [Function documentation](#function-documentation).
- Never `TEST_ASSERT` while a test's fixture exists. Its abort skips the cleanup and leaves the fixture under `/tmp`. Guard each step with `if (!TEST_CHECK(...)) { goto cleanup; }` and remove the fixture at a single `cleanup` label, or collect results, remove the fixture, then check. Inside a fixture's lifetime this replaces the `TEST_ASSERT` guard on a standard-library pointer.
- Outside a fixture's lifetime, guard a pointer from a standard-library allocation (`fopen`, `open_memstream`, `tmpfile`, `calloc`) with `TEST_ASSERT` followed by an explicit `if (ptr == NULL) return;`. The assert states the precondition and the `if` is what static analysis can actually follow: `TEST_ASSERT` hides its abort behind a call whose return value `cppcheck` cannot reason about, so without the `if` it reports `nullPointerOutOfResources` on the next use, and a cross-translation-unit variant in whichever `src/` function receives the pointer. The `if` body is unreachable and does not unwind. An aborted test leaves descriptors and allocations to process exit. Project allocators such as `arena_alloc` are not modeled as allocating by `cppcheck` and need no such guard.
