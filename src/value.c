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

microlisp_status ml_value_equal(ml_state *s, mvalue a, mvalue b, int *out_equal) {
    /* Bound the recursion. Two 500k-deep nested lists used to SEGFAULT
     * here even though both structures were finite. The check is on
     * every invocation so cyclic structures also trip the limit
     * instead of recursing forever. */
    if (s->equal_depth >= s->max_equal_depth) {
        ml_set_error(s, 0, 0, "equal?: comparison depth exceeded limit of %zu", s->max_equal_depth);
        return MICROLISP_ERR_EQUAL_DEPTH;
    }
    if (ml_value_eqv(a, b)) {
        *out_equal = 1;
        return MICROLISP_OK;
    }
    if (ml_is_pair(a) && ml_is_pair(b)) {
        mpair *pa = ml_as_pair(a);
        mpair *pb = ml_as_pair(b);
        s->equal_depth++;
        int car_eq = 0;
        microlisp_status st = ml_value_equal(s, pa->car, pb->car, &car_eq);
        if (st != MICROLISP_OK) {
            s->equal_depth--;
            return st;
        }
        if (!car_eq) {
            s->equal_depth--;
            *out_equal = 0;
            return MICROLISP_OK;
        }
        int cdr_eq = 0;
        st = ml_value_equal(s, pa->cdr, pb->cdr, &cdr_eq);
        s->equal_depth--;
        if (st != MICROLISP_OK) {
            return st;
        }
        *out_equal = cdr_eq;
        return MICROLISP_OK;
    }
    *out_equal = 0;
    return MICROLISP_OK;
}
