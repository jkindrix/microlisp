## Summary

<!-- One or two sentences: what changes and why. -->

## Changes

- <!-- bullet list of user-visible changes -->

## Verification

- [ ] `cmake --preset debug && cmake --build --preset debug`
- [ ] `ctest --preset default`
- [ ] `ctest --preset asan` (or note why N/A)
- [ ] `scripts/format.sh` is a no-op
- [ ] `scripts/lint.sh` is clean
- [ ] Public-API changes are reflected in `tests/check_exports.sh`'s
      expected list AND in `README.md`'s API synopsis
- [ ] `CHANGELOG.md` updated under `## [Unreleased]`

## Linked issues

<!-- Fixes #123 / Refs #456 -->
