# Changelog

All notable changes are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

### Changed

### Fixed

### Removed

## [0.1.0] - 2026-05-16

Initial public release. A small Scheme-subset interpreter with
mark-and-sweep GC, proper tail-call optimization, and the same
production-grade scaffolding as the rest of the trajectory.

### Added

- **Public C API** (`include/microlisp/microlisp.h`): nine entry points
  for state lifecycle (`microlisp_state_create`, `_destroy`), evaluation
  (`microlisp_eval`, `microlisp_repl`, `microlisp_buffer_free`),
  diagnostics (`microlisp_state_error`, `_error_position`,
  `microlisp_status_string`), and identity (`microlisp_version`).
- **Reader**: integers, booleans (`#t`/`#f`), symbols, strings (with
  `\n \t \r \" \\ \0 \xHH` escapes), proper and dotted-pair lists,
  the `'expr` quote shorthand, and line comments (`;`).
- **Evaluator**: trampolined main loop with proper tail-call
  optimization. Tail-recursive Scheme programs run in O(1) C-stack
  space.
- **Special forms**: `quote`, `if`, `define` (value + lambda-sugar),
  `lambda` (positional, variadic with `. rest`, rest-only),
  `set!`, `let`, `let*`, `letrec`, `begin`, `and`, `or`, `cond`
  (with `=>` deferred).
- **Built-in primitives**:
  arithmetic (`+ - * / quotient remainder modulo`, all overflow-checked),
  comparison (`= < > <= >=`, chain-style),
  list operations (`cons car cdr list length`),
  predicates (`null? pair? number? symbol? string? procedure? boolean? eq? eqv? equal? not`),
  I/O (`display write newline error`).
- **Value representation**: tagged pointers (low-3-bit tag on
  `uintptr_t`), with fixnums in 60-bit signed range. Out-of-range
  integer arithmetic yields `MICROLISP_ERR_OVERFLOW`.
- **Garbage collector**: mark-and-sweep over a linked list of all
  heap objects. Roots are the top-level environment plus a per-state
  protect stack used by the reader, evaluator, and printer to keep
  freshly-allocated handles alive across subsequent allocations.
- **Reader and evaluator depth limits** (`MICROLISP_ERR_READ_DEPTH`
  and `MICROLISP_ERR_EVAL_DEPTH`) to keep the host C stack bounded
  under hostile input. Tail-recursive code is unaffected by either
  limit.
- **Printer**: `display` (human-readable) and `write` (read-back-safe
  with quoted strings and the same `\xHH` escapes the reader accepts).
- **CLI** (`microlisp`): interactive REPL, `microlisp FILE` script
  runner, `microlisp -e EXPR` one-shot eval, conventional exit codes
  (0 OK, 1 eval error, 2 I/O / usage), SIGPIPE handling so a closed
  downstream pipe surfaces as exit 2 rather than 141.
- **Build system**: target-based CMake ≥ 3.20 with installed-as-package
  + in-tree-subproject + FetchContent support, compile + link hardening
  on non-Debug builds (`_FORTIFY_SOURCE`, stack protector,
  `-ffile-prefix-map`, PIE + RELRO + BIND_NOW on Linux executables),
  CMake presets for debug / release / relwithdebinfo / asan / tsan /
  msan / coverage.
- **Tests**: 40 in-file unit tests; 11 CTest-driven CLI smoke tests
  including SIGPIPE behaviour; the unit harness covers reader,
  evaluator, primitives, TCO, GC stress, and roundtrip-via-quote
  property tests. Single-source-of-truth sanitizer environment baked
  into CTest properties.
- **Fuzz harnesses** (libFuzzer): `fuzz_read` runs against a seed
  corpus and a token dictionary on every CI push; `fuzz_eval` is
  buildable for local exploration.
- **Policy-as-test**: `check_exports.sh` asserts the shared library's
  dynamic symbol table equals exactly the documented set;
  `check_doxygen_coverage.sh` asserts every `MICROLISP_API`
  declaration carries a Doxygen `/** ... */` block. Both ship with
  negative tests so the checkers themselves don't silently pass.
- **Install + downstream-consumer integration test**: CI installs
  the project to a staging prefix and rebuilds a separate consumer
  CMake project against it via `find_package(microlisp 0.1 REQUIRED)`,
  proving the install pipeline works end-to-end.
- **CI**: build matrix over {Ubuntu, macOS, Windows} × {gcc, clang,
  msvc} × {debug, release}, plus dedicated jobs for ASan+UBSan, TSan,
  MSan, lint (clang-format + clang-tidy with `-Werror=*`), coverage
  (≥ 75 % line-coverage floor), shared-build (which runs the
  symbol-export checks), install+consumer, fuzz smoke, and Doxygen
  build + Pages deploy on `main`. CodeQL `security-and-quality`
  weekly.
- **Repository hygiene**: SPDX-License-Identifier headers, `.editorconfig`,
  `.clang-format`, `.clang-tidy` with annotated suppressions,
  `.gitattributes`, pre-commit format hook, `scripts/format.sh` /
  `lint.sh` / `coverage.sh` / `install-hooks.sh`.

[Unreleased]: https://github.com/jkindrix/microlisp/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/jkindrix/microlisp/releases/tag/v0.1.0
