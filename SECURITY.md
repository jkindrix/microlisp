# Security Policy

## Supported versions

Only the latest minor release line receives security fixes.

| Version | Supported |
| ------- | --------- |
| 0.1.x   | ✅        |

## Reporting a vulnerability

Please **do not** open a public GitHub issue for suspected security
problems. Use a [private security advisory](https://github.com/jkindrix/microlisp/security/advisories/new)
with:

- A description of the issue and its impact (e.g., DoS via crafted
  input, out-of-bounds read, use-after-free in the GC).
- Steps to reproduce — ideally a minimal Scheme program (or byte
  sequence to feed the reader), plus the build configuration (Debug
  / Release / sanitizer / fuzzer).
- Affected version(s) and platform(s).

You will receive an acknowledgment within 72 hours. Once a fix is
available, we will coordinate a disclosure date with you and publish a
patched release.

## Threat model and limits

`microlisp` v0.1 is suitable for **embedding-with-trusted-input** use
cases (configuration, scripting that you wrote yourself). The
library applies the following defenses to keep the blast radius from
malformed or hostile input bounded:

- Reader depth limit (default 256 nested parens) → `MICROLISP_ERR_READ_DEPTH`.
- Evaluator depth limit (default 1024 non-tail-recursive frames) →
  `MICROLISP_ERR_EVAL_DEPTH`. Tail-recursive code runs in O(1) C-stack
  space regardless.
- Integer overflow on every arithmetic primitive → `MICROLISP_ERR_OVERFLOW`.
- Strict GC discipline with mark-and-sweep; libFuzzer + ASan + UBSan
  on every push smoke-test the reader against arbitrary bytes.

However, the language is Turing-complete and v0.1 has **no
instruction-count limit**. A loop like `(define (loop) (loop)) (loop)`
runs forever (in constant memory thanks to TCO). Embedders accepting
input from untrusted sources should run evaluation in a worker
thread with their own watchdog, or wait for a future minor release
that adds an instruction-count budget.

## Hardening

The library is built with the following hardening features:

Compile-time (non-Debug):

- `_FORTIFY_SOURCE=3` when the toolchain supports it; fallback `=2`.
  Detection is performed at configure time via `check_c_source_compiles`.
- `-fstack-protector-strong`.
- `-fstrict-flex-arrays=3` when the toolchain supports it.
- `-fvisibility=hidden` plus an explicit `MICROLISP_API` export macro.
- `-ffile-prefix-map` to strip absolute build paths from `__FILE__`
  strings and DWARF info.

Link-time (Linux executables):

- `-pie` (position-independent executables).
- `-Wl,-z,relro -Wl,-z,now` (full RELRO + immediate binding).
- `-Wl,-z,noexecstack`.

CI exercises every change under AddressSanitizer + UndefinedBehaviorSanitizer,
ThreadSanitizer, and MemorySanitizer (each in its own job, with
`halt_on_error=1` and `detect_leaks=1` where applicable). A libFuzzer
harness (`fuzz_read`) exercises the reader on every push with a seed
corpus and token dictionary; a second harness (`fuzz_eval`) is
buildable for local exploration but not run in CI smoke (Turing-complete
inputs can legitimately exceed any per-input timeout). CodeQL's
`security-and-quality` query suite runs weekly plus on every push/PR.
