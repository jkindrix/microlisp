#!/usr/bin/env bash
# Run the full coverage cycle: reset stale profile data, run the
# coverage-build test suite, capture coverage with flags appropriate to
# the installed lcov, and enforce the line-coverage floor.
#
# Owning all four steps in one script (rather than relying on the caller
# to remember the reset) prevents stale .gcda files from corrupting the
# capture or leaking libgcov stderr noise into ctest output -- the bug
# the v0.1.4 cold review caught.
#
# CI uses lcov 2.x's stricter flag surface; older distros (Debian 12
# ships lcov 1.16) reject those flags. The capture step auto-detects
# the version.
#
# Usage: scripts/coverage.sh [BUILD_DIR]   (default: build/coverage)
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build/coverage}"
# 75% is the v0.x baseline. Override via MIN_LINE_COVERAGE=<pct> when
# investigating a regression or when ratcheting up after adding more
# end-to-end tests.
FLOOR="${MIN_LINE_COVERAGE:-75}"

if ! command -v lcov >/dev/null 2>&1; then
    echo "error: lcov not found in PATH" >&2
    exit 1
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "error: $BUILD_DIR does not exist." >&2
    echo "       Run: cmake --preset coverage && cmake --build --preset coverage" >&2
    exit 1
fi

# Reset stale .gcda. Without this, re-running ctest against an existing
# coverage build appends to old per-run counters; the merged data
# confuses libgcov, which spams stderr ("Merge mismatch for ..." etc.)
# and breaks CLI regex tests that inspect stdout/stderr layout.
find "$BUILD_DIR" -name '*.gcda' -delete

# Run the test suite. This is what produces the .gcda files we'll
# capture next.
ctest --test-dir "$BUILD_DIR" --output-on-failure

lcov_major=$(lcov --version 2>&1 | sed -nE 's/.*LCOV version ([0-9]+).*/\1/p' | head -1)

capture_args=(--capture --directory "$BUILD_DIR" --output-file "$BUILD_DIR/coverage.info")
remove_args=(--remove "$BUILD_DIR/coverage.info" '/usr/*' '*/tests/*' --output-file "$BUILD_DIR/coverage.info")

if [ "${lcov_major:-1}" -ge 2 ]; then
    # --ignore-errors mismatch: gcov reports end-lines that don't match
    # the function's source location when a function is defined inside
    # the harness's TEST() macro.
    # --ignore-errors unused: lcov 2.x's --remove errors when a glob
    # matches zero files; /usr/* is defensive on some runner images.
    capture_args+=(--ignore-errors mismatch)
    remove_args+=(--ignore-errors unused)
fi
# This project tracks **line coverage only**. Branch coverage would
# require additional -fprofile-arcs / -ftest-coverage plumbing beyond
# the basic --coverage flag, and is deferred until the line-coverage
# floor is comfortably above 90%.

lcov "${capture_args[@]}"
lcov "${remove_args[@]}"
lcov --list "$BUILD_DIR/coverage.info"

pct=$(lcov --summary "$BUILD_DIR/coverage.info" 2>&1 \
       | awk '/^ *lines\.+:/ {gsub("%",""); print $2; exit}')
if [ -z "$pct" ]; then
    echo "error: could not parse line coverage from lcov --summary" >&2
    exit 1
fi
echo "line coverage: ${pct}% (floor: ${FLOOR}%)"
if ! awk -v p="$pct" -v f="$FLOOR" 'BEGIN { exit (p+0 >= f+0) ? 0 : 1 }'; then
    echo "error: line coverage ${pct}% < ${FLOOR}%" >&2
    exit 1
fi
