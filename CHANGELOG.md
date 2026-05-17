# Changelog

All notable changes are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

### Changed

### Fixed

### Removed

## [0.1.2] - 2026-05-16

Second post-release iteration. Five findings from a second cold
review, all landed.

### Fixed

- **Malformed dotted special forms no longer crash.** `(begin . 1)`,
  `(and . 1)`, `(or . 1)`, and `(cond (#t . 1))` previously cast a
  non-pair tail to a pair pointer inside `begin_tco` / `handle_and_or`
  and SEGFAULTed under ASan. The three handlers now validate the
  argument list (and cond clauses' bodies via begin_tco) as proper
  lists up front and return `MICROLISP_ERR_SYNTAX` on improper input.
- **`(/ -2^59 -1)` and `(quotient -2^59 -1)` correctly report overflow.**
  Both checks were guarding `INT64_MIN / -1` (an int64 overflow) but
  the actual fixnum boundary is `M_FIX_MIN / -1 = M_FIX_MAX + 1`,
  one past the documented 60-bit range. The result is now an explicit
  `MICROLISP_ERR_OVERFLOW` rather than a wrong-but-tagged value.
- **A lone `'` at end-of-input rejects cleanly.** `microlisp -e "'"`
  previously printed the EOF sentinel because the reader silently
  accepted `MV_EOF` from the inner `read_form`. It now returns
  `MICROLISP_ERR_READ_TRUNCATED` with a position-aware diagnostic.
- **`(let ((x 1 2)) x)` errors instead of silently returning 1.**
  The binding validator only checked that the binding had two cells;
  any extra cells were ignored. Now requires exactly `(name expr)`
  with `cdr.cdr == nil`. Applies to `let`, `let*`, and `letrec`.

### Changed

- **MSVC support claim walked back.** The README previously listed
  MSVC ≥ 2019 in the requirements; the library uses GCC/Clang
  `__builtin_*_overflow` intrinsics directly and has no MSVC fallback.
  The Windows CI matrix entry labelled `msvc` actually runs MinGW
  gcc. README and troubleshooting table updated to reflect reality;
  proper MSVC support (via SafeInt or equivalent) is on the v0.2
  roadmap.

## [0.1.1] - 2026-05-16

First post-release iteration. Picks up the highest-ROI items from the
v0.1.0 cold review; the heavier roadmap (`microlisp_state_interrupt`,
the value-level C API, hygienic macros, NOMEM fault injection,
coverage ratchet) is held for 0.2.

### Fixed

- **GC mark walker is now iterative.** The v0.1.0 recursive
  `mark_obj` overflowed the C stack on any heap-built linked list
  of more than ~3000 cons cells -- exactly the shape a tail-
  recursive `(loop n (cons n acc))` produces. Replaced with an
  explicit worklist held on the state; the worklist grows
  on-demand and a grow-failure cleanly aborts the collection
  rather than half-marking the heap. Found by fuzz_read shortly
  after that target landed in CI (see Added section below for
  the harness split that made fuzz_read's findings interpretable).
  Regression test: a 100 000-cell list now traverses cleanly under
  ASan.

### Added

- **`fuzz_eval` runs in CI** as part of the libFuzzer-smoke job, with
  `-timeout_exitcode=0` plus a wall-time + RSS envelope and a shell
  wrapper that treats `libFuzzer: out-of-memory` / `libFuzzer:
  timeout` as non-failures. Real ASan / UBSan / assertion findings
  still propagate. The wrapper exists because the evaluator is
  Turing-complete: a fuzzer-generated `(define (loop) (loop)) (loop)`
  legitimately exceeds any per-input budget, and the only valid
  CI signal for that is "the input finished cleanly OR hit the
  documented envelope" -- not "the input is a bug."
- **`fuzz_read` harness now drives the reader directly** (via
  internal `ml_read`) instead of going through `microlisp_eval`.
  The reader is non-Turing-complete -- its runtime is bounded by
  input size -- so any timeout or OOM in fuzz_read is now a real
  bug worth investigating.
- **Eight new fuzz seeds** covering tail-call accumulator loops,
  letrec mutual recursion, closure-captured state, cond chains,
  short-circuit and/or, string-escape edge cases, dotted-pair
  alists, and a higher-order sum-of-squares fold.
- **Twelve new entries in the libFuzzer dictionary** (`lambda (`,
  `define (`, `(if `, `(let ((`, `(letrec ((`, `(cond (`, ` . `,
  ` else`, `()`, ` `, `#t`, `#f`) so the mutator splices format-
  aware tokens into inputs more aggressively.
- **`gc_mark_walks_long_list_iteratively`** unit test pins the GC
  regression: a 100 000-element list survives a mark cycle.

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

[Unreleased]: https://github.com/jkindrix/microlisp/compare/v0.1.2...HEAD
[0.1.2]: https://github.com/jkindrix/microlisp/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/jkindrix/microlisp/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/jkindrix/microlisp/releases/tag/v0.1.0
