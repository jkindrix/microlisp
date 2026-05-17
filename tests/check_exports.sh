#!/usr/bin/env bash
# Assert that libmicrolisp.so's dynamic symbol table is exactly the
# documented public MICROLISP_API surface. Catches accidental leakage of
# private helpers (forgot to mark `static`, forgot to gate a helper that
# slipped into the header, etc.).
#
# Argument: path to libmicrolisp.so.
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <path-to-libmicrolisp.so>" >&2
    exit 2
fi
lib="$1"

if ! command -v nm >/dev/null 2>&1; then
    echo "check_exports: nm not found in PATH; skipping" >&2
    exit 0
fi

# Keep this list sorted; it's diffed against `nm`'s output.
expected=$(printf '%s\n' \
    microlisp_buffer_free \
    microlisp_eval \
    microlisp_repl \
    microlisp_state_create \
    microlisp_state_destroy \
    microlisp_state_error \
    microlisp_state_error_position \
    microlisp_status_string \
    microlisp_version \
    | sort)

actual=$(nm -D --defined-only --format=posix "$lib" \
    | awk '{print $1}' \
    | sed 's/@.*//' \
    | sort -u)

diff_out=$(diff <(echo "$expected") <(echo "$actual") || true)
if [ -n "$diff_out" ]; then
    echo "FAIL: libmicrolisp.so exports an unexpected dynamic symbol set." >&2
    echo "      Lines starting with '<' are documented-but-missing," >&2
    echo "      '>' are present-but-undocumented." >&2
    echo "$diff_out" >&2
    exit 1
fi

count=$(echo "$expected" | wc -l)
echo "ok: dynamic symbol table = ${count} expected microlisp_* exports"
