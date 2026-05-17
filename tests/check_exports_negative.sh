#!/usr/bin/env bash
# Negative test for check_exports.sh: build a synthetic shared library with
# a leaked symbol and verify the check correctly fails on it.
#
# Without this, a silent-pass bug in check_exports.sh would never be
# caught -- the positive test would still report "ok" against a real
# library while masking that the check itself is broken.
#
# Argument: path to check_exports.sh to test.
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <path-to-check_exports.sh>" >&2
    exit 2
fi
checker="$1"

if ! command -v cc >/dev/null 2>&1; then
    echo "check_exports_negative: cc not in PATH; skipping" >&2
    exit 0
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

cat > "$tmpdir/lib.c" <<'EOF'
int microlisp_buffer_free(void){return 0;}
int microlisp_eval(void){return 0;}
int microlisp_repl(void){return 0;}
int microlisp_state_create(void){return 0;}
int microlisp_state_destroy(void){return 0;}
int microlisp_state_error(void){return 0;}
int microlisp_state_error_position(void){return 0;}
int microlisp_status_string(void){return 0;}
int microlisp_version(void){return 0;}
int microlisp_accidentally_public(void){return 42;}
EOF

cc -shared -fPIC -o "$tmpdir/libfake.so" "$tmpdir/lib.c"

set +e
out=$(bash "$checker" "$tmpdir/libfake.so" 2>&1)
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
    echo "FAIL: check_exports.sh accepted a library with a leaked symbol" >&2
    echo "      output: $out" >&2
    exit 1
fi
if ! echo "$out" | grep -q 'microlisp_accidentally_public'; then
    echo "FAIL: check_exports.sh rejected the library but didn't name the leak" >&2
    echo "      output: $out" >&2
    exit 1
fi

echo "ok: check_exports.sh correctly rejected a library with a leaked symbol"
