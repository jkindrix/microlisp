#!/usr/bin/env bash
# Negative test for check_doxygen_coverage.sh.
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <path-to-check_doxygen_coverage.sh>" >&2
    exit 2
fi
checker="$1"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

cat > "$tmpdir/bad.h" <<'EOF'
#ifndef BAD_H
#define BAD_H

/** documented */
MICROLISP_API int documented_function(void);

MICROLISP_API int undocumented_function(void);

#endif /* BAD_H */
EOF

set +e
out=$(bash "$checker" "$tmpdir/bad.h" 2>&1)
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
    echo "FAIL: check_doxygen_coverage.sh accepted a header with an undocumented MICROLISP_API decl" >&2
    echo "      output: $out" >&2
    exit 1
fi
if ! echo "$out" | grep -q 'undocumented_function'; then
    echo "FAIL: check_doxygen_coverage.sh rejected the header but didn't name the offender" >&2
    echo "      output: $out" >&2
    exit 1
fi

echo "ok: check_doxygen_coverage.sh correctly rejected an undocumented declaration"
