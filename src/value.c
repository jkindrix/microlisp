/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * Value-equality helpers used across reader, evaluator, and primitives.
 *
 * Tag-level identity (`eq?`) is just integer equality on the mvalue.
 * Structural equality (`equal?`) walks pairs and strings recursively.
 * `eqv?` extends `eq?` to also handle equal fixnums and equal strings
 * by content; in this v0.1 implementation all numbers and symbols
 * already compare equal under `eq?` (fixnums are tagged immediates,
 * symbols are interned), so `eqv?` and `eq?` differ only on strings.
 */

#include "microlisp_internal.h"

#include <string.h>

int ml_value_eq(mvalue a, mvalue b) {
    return a == b;
}

int ml_value_eqv(mvalue a, mvalue b) {
    if (a == b) {
        return 1;
    }
    if (ml_is_string(a) && ml_is_string(b)) {
        mstring *sa = ml_as_string(a);
        mstring *sb = ml_as_string(b);
        return sa->len == sb->len && (sa->len == 0 || memcmp(sa->bytes, sb->bytes, sa->len) == 0);
    }
    return 0;
}

int ml_value_equal(mvalue a, mvalue b) {
    if (ml_value_eqv(a, b)) {
        return 1;
    }
    if (ml_is_pair(a) && ml_is_pair(b)) {
        mpair *pa = ml_as_pair(a);
        mpair *pb = ml_as_pair(b);
        return ml_value_equal(pa->car, pb->car) && ml_value_equal(pa->cdr, pb->cdr);
    }
    return 0;
}
