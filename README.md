# microlisp

[![CI](https://github.com/jkindrix/microlisp/actions/workflows/ci.yml/badge.svg)](https://github.com/jkindrix/microlisp/actions/workflows/ci.yml)
[![CodeQL](https://github.com/jkindrix/microlisp/actions/workflows/codeql.yml/badge.svg)](https://github.com/jkindrix/microlisp/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C17](https://img.shields.io/badge/C-17-informational.svg)](https://www.iso.org/standard/74528.html)
[![CMake >= 3.20](https://img.shields.io/badge/CMake-%E2%89%A5%203.20-informational.svg)](CMakeLists.txt)

A small Scheme-subset interpreter in modern C. Embeddable as a library
or used standalone via a CLI REPL.

The language is a strict R5RS-flavored subset: integers (60-bit signed),
booleans, symbols, strings, pairs, `'()` (nil), and procedures
(closures and built-ins). Special forms cover `quote`, `if`, `define`,
`lambda`, `set!`, `let`, `let*`, `letrec`, `begin`, `and`, `or`, and
`cond`. Built-in primitives cover arithmetic, comparison, list
construction and accessors, the standard predicate vocabulary, and
`display` / `write` / `newline` / `error`.

The implementation uses **tagged pointers** for the value
representation (low-3-bit tag on `uintptr_t`) and a **mark-and-sweep
garbage collector**. **Proper tail-call optimization** is provided by
a trampolined evaluator -- tail-recursive Scheme programs run in O(1)
C-stack space.

## What's in the box

- **A reader** that turns Scheme source into S-expressions. Handles
  proper and dotted-pair lists, the `'expr` quote shorthand, string
  literals with the usual newline / tab / quote / hex-byte escapes, and `;`
  line comments. Configurable nesting-depth limit to bound stack use
  on hostile input.
- **A trampolined evaluator** with proper tail-call optimization,
  full lexical scoping, first-class closures, `set!`-able bindings,
  and a configurable non-tail-recursion depth ceiling.
- **A mark-and-sweep GC** with a per-state protect stack used by the
  reader, evaluator, and printer to keep handles alive across nested
  allocations.
- **A small CLI** (`microlisp`) for REPL, `-e EXPR`, and script-file
  use. Conventional exit codes; SIGPIPE handling so closed pipes
  surface as exit 2.
- **Strict error reporting.** Every off-the-happy-path case has a
  specific error code (unbound variable, type mismatch, arity
  mismatch, integer overflow, division by zero, reader nesting too
  deep, evaluator recursion too deep, ...) and a human-readable
  message reachable via `microlisp_state_error`.

## Requirements

- A 64-bit, C17-capable compiler (GCC ≥ 9, Clang ≥ 9, MSVC ≥ 2019).
- CMake ≥ 3.20.
- Ninja recommended.

## Build & run

```sh
cmake --preset debug
cmake --build --preset debug
./build/debug/microlisp -e '(+ 1 2 3)'                    # 6
./build/debug/microlisp -e '(define (f n) (if (= n 0) 1 (* n (f (- n 1))))) (f 10)'
# 3628800
./build/debug/microlisp                                    # interactive REPL
```

Example session:

```text
$ ./build/debug/microlisp
microlisp> (define (sq x) (* x x))
microlisp> (sq 9)
81
microlisp> (define xs (list 1 2 3 4 5))
microlisp> (define (sum xs acc) (if (null? xs) acc (sum (cdr xs) (+ acc (car xs)))))
microlisp> (sum xs 0)
15
microlisp> ^D
```

Other presets:

```sh
cmake --preset release         && cmake --build --preset release
cmake --preset asan            && cmake --build --preset asan
cmake --preset tsan            && cmake --build --preset tsan
cmake --preset msan            && cmake --build --preset msan     # CC=clang required
cmake --preset coverage        && cmake --build --preset coverage
```

## CLI

| Invocation                 | Behavior                                                    |
|----------------------------|-------------------------------------------------------------|
| `microlisp`                | Interactive REPL on stdin.                                  |
| `microlisp FILE`           | Evaluate FILE as a script (top-level forms in order).       |
| `microlisp -`              | Same, reading the script from stdin.                        |
| `microlisp -e EXPR`        | Evaluate EXPR and print the result.                         |
| `microlisp --help`         | Usage summary.                                              |
| `microlisp --version`      | Print `microlisp X.Y.Z`.                                    |

Exit codes:

| Code | Meaning                                                                       |
|------|-------------------------------------------------------------------------------|
| 0    | Success.                                                                      |
| 1    | An evaluation error (reader, type, arity, unbound, overflow, depth, user `error`). |
| 2    | I/O error reading the input or writing to stdout, or an unknown option.       |

## Test

```sh
ctest --preset default
```

The suite covers reader edge cases (integer overflow, deep nesting,
unmatched parens, malformed escapes), arithmetic overflow and
division by zero, comparison chaining, every special form, closures
that capture mutable state, variadic lambdas, `let`/`let*`/`letrec`
semantics, predicate vocabulary, every error path on the public API,
TCO at 200 000 tail calls, GC stress over thousands of allocations,
the non-tail recursion depth ceiling, and a print/parse roundtrip
property test.

## Install

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix /usr/local
```

Installs:

- `bin/microlisp`            — the CLI / REPL
- `lib/libmicrolisp.a`       — the static library (or `.so` with `BUILD_SHARED_LIBS=ON`)
- `include/microlisp/*.h`    — public headers
- `lib/cmake/microlisp/`     — `find_package(microlisp)` config files
- `lib/pkgconfig/microlisp.pc` — relocatable pkg-config metadata

## Consuming

```cmake
find_package(microlisp 0.1 REQUIRED)
target_link_libraries(my_app PRIVATE microlisp::microlisp)
```

Or as a subproject:

```cmake
add_subdirectory(third_party/microlisp)
target_link_libraries(my_app PRIVATE microlisp::microlisp)
```

Non-CMake consumers can use `pkg-config`:

```sh
cc my_app.c $(pkg-config --cflags --libs microlisp) -o my_app
```

## Public API at a glance

```c
#include <microlisp/microlisp.h>

/* State lifecycle */
microlisp_status microlisp_state_create(const microlisp_options *opts,
                                        microlisp_state **out);
void             microlisp_state_destroy(microlisp_state *state);

/* Evaluation */
microlisp_status microlisp_eval(microlisp_state *state,
                                const char *source, size_t source_len,
                                char **out_bytes, size_t *out_len);
microlisp_status microlisp_repl(microlisp_state *state,
                                FILE *in_file, FILE *out_file,
                                const char *prompt);
void             microlisp_buffer_free(microlisp_state *state, char *bytes);

/* Diagnostics */
const char      *microlisp_state_error(const microlisp_state *state);
void             microlisp_state_error_position(const microlisp_state *state,
                                                size_t *out_line,
                                                size_t *out_column);

/* Misc */
const char      *microlisp_version(void);
const char      *microlisp_status_string(microlisp_status status);
```

Each `microlisp_state` is **MT-Unsafe** (caller serializes); distinct
states may be used concurrently from different threads. The full
per-function contracts (allocator hook, depth limits, error
semantics) live in [`include/microlisp/microlisp.h`](include/microlisp/microlisp.h).

## Versioning & ABI stability

`microlisp` follows [Semantic Versioning](https://semver.org/):

- **Major** (`X.0.0`) — may break the public C API or ABI.
- **Minor** (`1.X.0`) — adds API surface without breaking existing
  callers. Shared-library `SOVERSION` is preserved.
- **Patch** (`1.0.X`) — bug fixes and documentation only.

**0.x exception.** Until 1.0.0, this project follows the standard
SemVer convention that the 0.x series is unstable. In particular,
**patch releases during 0.x may add new error-enum values or other
source-compatible additions** when needed to diagnose a fixed bug.
Strict consumers compiling with `-Wswitch-enum -Werror` should pin to
an exact `0.x.y` during this phase. The minor/patch discipline above
kicks in once 1.0.0 ships.

The public surface is exactly the symbols declared in
`include/microlisp/microlisp.h` and tagged with `MICROLISP_API`.
Anything else is private. The `shared-build` CI job runs a
`symbol_export_check` test on Linux that fails the build if a private
symbol leaks into the dynamic table.

## Scope and non-goals (v0.1)

`microlisp` 0.1 is a small, careful Scheme subset. It is **not**
trying to be R5RS, R6RS, or R7RS. Specifically, the following are
out of scope for v0.1 and may land in a future minor release:

- Floats, rationals, complex numbers (integers are 60-bit signed).
- Characters (Scheme's `#\\name` literal syntax), vectors (`#(...)`), bytevectors.
- Macros (`define-syntax`, `syntax-rules`, hygienic anything).
- Continuations (`call/cc`, `dynamic-wind`).
- Tail-position guarantees beyond what R5RS requires.
- An instruction-count budget for the evaluator (so a `(define (loop) (loop)) (loop)`
  runs forever in constant space). Use a watchdog thread if you accept
  untrusted input.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `find_package(microlisp)` not found. | Install prefix isn't on CMake's search path. | `-DCMAKE_PREFIX_PATH=PREFIX` or set `microlisp_DIR`. |
| MSVC shared-link unresolved externals. | Consumer compiled without `dllimport`. | Define `MICROLISP_USE_SHARED` before including the public header. |
| `pkg-config --cflags --libs microlisp` not found. | Install prefix's pkgconfig dir isn't on `PKG_CONFIG_PATH`. | `export PKG_CONFIG_PATH=PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH`. |
| Sanitizer build aborts with `unexpected memory mapping`. | Kernel ≥ 6.x with Clang ≤ 15: ASLR vs sanitizer shadow. | Use Clang ≥ 16 (e.g. `CC=clang-19 cmake --preset asan`), or `sudo sysctl -w vm.mmap_rnd_bits=28`. Details in [CONTRIBUTING.md](CONTRIBUTING.md). |
| Fuzz target fails to link with `libclang_rt.fuzzer-*.a: No such file`. | Debian splits Clang's sanitizer/fuzzer runtimes. | `sudo apt install -y libclang-rt-19-dev` (matching your `clang` version). |

## Development

- Format: `scripts/format.sh` runs `clang-format -i` on all sources.
- Lint:   `scripts/lint.sh` runs `clang-tidy` against `compile_commands.json`.
- Hooks:  `scripts/install-hooks.sh` wires `core.hooksPath` to `.githooks/`,
  enabling a `clang-format --dry-run -Werror` check on staged C sources
  at commit time.
- Coverage: `scripts/coverage.sh` computes coverage and enforces the
  v0.x 75 % floor (overridable with `MIN_LINE_COVERAGE=PCT`), with
  lcov 1.x/2.x version detection. The floor will be ratcheted up as
  fault-injection tests are added for the NOMEM / I/O error paths.
- Fuzz:    `cmake --build build/fuzz --target fuzz_read fuzz_eval`
  then `./build/fuzz/tests/fuzz/fuzz_read tests/fuzz/corpus -dict=tests/fuzz/microlisp.dict -max_total_time=60`.
- CI:      see `.github/workflows/ci.yml` for the build/test matrix,
  sanitizer (ASan+UBSan, TSan, MSan), lint, coverage, fuzz, downstream
  consumer, and Doxygen jobs.

## License

[MIT](LICENSE).
