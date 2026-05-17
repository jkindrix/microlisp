# Contributing

Thanks for considering a contribution! Please read this before opening a
pull request.

## One-time maintainer setup

Three repository settings have to be flipped by hand the first time
this project is published; the CI pipeline and the issue/support
workflows assume all three are in place:

- **Settings → Pages → Source: GitHub Actions.** The `pages` job in
  `.github/workflows/ci.yml` deploys the Doxygen site on every push to
  `main` and will fail until this is set.
- **Settings → Code security → Code scanning: enable CodeQL.** Required
  before `.github/workflows/codeql.yml` can upload SARIF results.
- **Settings → General → Features → Discussions: enable.**
  [SUPPORT.md](SUPPORT.md) and `.github/ISSUE_TEMPLATE/config.yml` both
  redirect how-to / design questions to Discussions; the links 404
  until the feature is on.

## Governance

This is a single-maintainer project ([`@jkindrix`](https://github.com/jkindrix)).
Pull requests are merged when:

1. CI is green on every required job (the full workflow under
   `.github/workflows/ci.yml` plus the CodeQL workflow).
2. The maintainer has approved the PR.
3. There are no unresolved review threads.

Releases are cut when the `[Unreleased]` section of
[CHANGELOG.md](CHANGELOG.md) accumulates a meaningful set of changes,
or when a critical fix lands. Conduct in any project space is governed
by the [Code of Conduct](CODE_OF_CONDUCT.md).

## Ground rules

- **Discuss before large changes.** Open an issue first for anything
  beyond a bug fix or doc tweak.
- **Keep changes focused.** One logical change per PR.
- **Write tests** for new behavior or bug fixes. CI must be green.
- **Follow the style.** Run `scripts/format.sh` and `scripts/lint.sh`
  before pushing.

## Building & testing

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset default
```

Run the sanitizer build at least once before opening a PR:

```sh
cmake --preset asan && cmake --build --preset asan
ctest --preset asan
```

### Toolchain version drift between local and CI

CI runs against the tools shipped on GitHub's hosted runners. If your
local distribution is older, `scripts/lint.sh` and the `coverage`
preset may report clean locally and still fail CI. The cases that
have actually bitten C-library projects of this shape:

| Concern | What CI runs | What older distros ship | Mitigation |
| --- | --- | --- | --- |
| `clang-tidy` | ≥ 18 | Debian 12 ships 14 | `sudo apt install clang-tidy-19`; run `BUILD_DIR=build/debug clang-tidy-19 ...` (or let CI catch it on the PR). |
| `lcov` | 2.x | Debian 12 ships 1.16 | Run `scripts/coverage.sh` instead of hand-copying CI commands — it auto-detects your lcov major version. |
| `head`, `stdbuf` | GNU coreutils on Linux runners; BSD on macOS | GNU on most Linux | Linux-only CTest entries are gated with `CMAKE_SYSTEM_NAME STREQUAL "Linux"` rather than `CMAKE_HOST_UNIX`. |

If you can, run the same compiler family CI uses (`CC=clang-19`).

### Sanitizers on newer Linux kernels (WSL2, kernel ≥ 6.x)

Linux ≥ 6.x defaults `vm.mmap_rnd_bits` to 32, which collides with the
shadow memory layout used by Clang ≤ 15's ASan / TSan / MSan /
libFuzzer runtimes. Symptoms: `FATAL: ...Sanitizer: unexpected memory
mapping` or sporadic SEGFAULTs.

- **Use Clang ≥ 16** for sanitizer/fuzz builds:

  ```sh
  sudo apt install -y clang-19 clang-tools-19 libclang-rt-19-dev
  CC=clang-19 cmake --preset asan
  CC=clang-19 cmake --preset tsan
  CC=clang-19 cmake --preset msan
  CC=clang-19 cmake -S . -B build/fuzz -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug -DMICROLISP_BUILD_FUZZERS=ON
  ```

- **Or lower the ASLR entropy at runtime** (doesn't persist across reboots):

  ```sh
  sudo sysctl -w vm.mmap_rnd_bits=28
  ```

CI's `ubuntu-latest` runners are unaffected.

## Commit messages

Use [Conventional Commits](https://www.conventionalcommits.org/):

```
feat(reader): accept #\space character literal
fix(gc): mark closure body before collecting
docs(readme): clarify TCO scope (tail position only)
test(eval): cover MICROLISP_ERR_EVAL_DEPTH boundary
refactor(env): extract symbol-search helper
ci: bump clang-tidy gate to v19
```

Subject ≤ 72 characters; body explains *why* when not obvious.

## Pull-request checklist

- [ ] Builds clean under `cmake --preset debug` and `--preset release`.
- [ ] `ctest --preset default` passes.
- [ ] `ctest --preset asan` passes (no ASan/UBSan diagnostics).
- [ ] `scripts/format.sh` is a no-op (code already formatted).
- [ ] `scripts/lint.sh` reports no new warnings.
- [ ] Public-API changes are reflected in `tests/check_exports.sh`'s
      expected list and in `README.md`'s "Public API at a glance".
- [ ] New behavior is documented in headers and in the README where
      user-facing.
- [ ] `CHANGELOG.md` updated under `## [Unreleased]`.
