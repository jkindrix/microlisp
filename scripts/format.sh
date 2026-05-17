#!/usr/bin/env bash
# Format every C source file in-place using the project's .clang-format.
set -euo pipefail

cd "$(dirname "$0")/.."

if ! command -v clang-format >/dev/null 2>&1; then
    echo "error: clang-format not found in PATH" >&2
    exit 1
fi

mapfile -t files < <(find include src tests examples \
    \( -name '*.c' -o -name '*.h' \) -not -path '*/build/*')

if [ ${#files[@]} -eq 0 ]; then
    echo "no source files found"
    exit 0
fi

clang-format -i "${files[@]}"
echo "formatted ${#files[@]} file(s)"
