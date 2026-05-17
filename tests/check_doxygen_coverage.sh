#!/usr/bin/env bash
# Assert that every MICROLISP_API declaration in the public header is
# preceded by a Doxygen /** ... */ comment block.
#
# Argument: path to microlisp.h.
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <path-to-microlisp.h>" >&2
    exit 2
fi
hdr="$1"

undoc=$(awk '
    BEGIN { in_doc = 0; doc_seen = 0 }
    /^[[:space:]]*\/\*\*.*\*\/[[:space:]]*$/ { doc_seen = 1; next }
    /^[[:space:]]*\/\*\*/ { in_doc = 1; next }
    in_doc && /\*\// { in_doc = 0; doc_seen = 1; next }
    in_doc { next }
    /^[[:space:]]*$/ { next }
    /^MICROLISP_API/ {
        if (!doc_seen) { print FILENAME ":" NR ": " $0 }
        doc_seen = 0
        next
    }
    { doc_seen = 0 }
' "$hdr")

if [ -n "$undoc" ]; then
    echo "FAIL: MICROLISP_API declaration without a preceding /** ... */ block:" >&2
    echo "$undoc" >&2
    exit 1
fi

count=$(grep -c '^MICROLISP_API' "$hdr" || true)
echo "ok: ${count} MICROLISP_API declaration(s) all carry a Doxygen comment"
