/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * Symbol interning.
 *
 * Symbols are byte sequences (typically ASCII identifiers but NUL bytes
 * are allowed for completeness with R5RS string<->symbol conversions
 * if we ever add them) identified by a stable integer index. Two
 * occurrences of the same symbol name in source produce mvalues that
 * compare equal as integers, so the evaluator can dispatch on symbol
 * identity without strcmp.
 *
 * v0.1 uses linear search for intern lookup. Programs with a few
 * hundred symbols see no measurable cost; tighter performance is a
 * v0.2 concern (open-addressing hash table with FNV-1a).
 */

#include "microlisp_internal.h"

#include <stdlib.h>
#include <string.h>

static int names_equal(const char *a, size_t a_len, const char *b, size_t b_len) {
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

microlisp_status ml_sym_intern(ml_state *s, const char *name, size_t len, mvalue *out) {
    if (name == NULL && len != 0) {
        return MICROLISP_ERR_INVALID_ARG;
    }

    /* Existing? */
    for (size_t i = 0; i < s->symtab.count; i++) {
        const ml_symtab_entry *e = &s->symtab.entries[i];
        if (names_equal(e->name, e->len, name, len)) {
            *out = ml_make_sym((uint32_t)i);
            return MICROLISP_OK;
        }
    }

    /* Grow? */
    if (s->symtab.count == s->symtab.capacity) {
        size_t new_cap = s->symtab.capacity == 0 ? 32 : s->symtab.capacity * 2;
        size_t bytes;
        if (__builtin_mul_overflow(new_cap, sizeof(ml_symtab_entry), &bytes)) {
            return MICROLISP_ERR_NOMEM;
        }
        ml_symtab_entry *grown = (ml_symtab_entry *)ml_raw_alloc(s, bytes);
        if (grown == NULL) {
            return MICROLISP_ERR_NOMEM;
        }
        if (s->symtab.entries != NULL) {
            memcpy(grown, s->symtab.entries, s->symtab.count * sizeof(ml_symtab_entry));
            ml_raw_free(s, s->symtab.entries);
        }
        s->symtab.entries = grown;
        s->symtab.capacity = new_cap;
    }

    /* Index ceiling for the tag layout: 60 bits is far more than any
     * real program will produce, but check defensively so an attacker
     * generating ever-fresh symbols can't roll into bits used by the
     * tag. */
    if (s->symtab.count > 0x0FFFFFFFU) {
        return MICROLISP_ERR_NOMEM;
    }

    /* Copy the name -- allocate len+1 so we can NUL-terminate (useful
     * for diagnostic prints and matches the "symbols are identifiers"
     * mental model). NUL bytes inside `name` are preserved verbatim;
     * the trailing NUL is purely decorative. */
    char *owned = (char *)ml_raw_alloc(s, len + 1);
    if (owned == NULL) {
        return MICROLISP_ERR_NOMEM;
    }
    if (len > 0) {
        memcpy(owned, name, len);
    }
    owned[len] = '\0';

    s->symtab.entries[s->symtab.count].name = owned;
    s->symtab.entries[s->symtab.count].len = len;
    *out = ml_make_sym((uint32_t)s->symtab.count);
    s->symtab.count++;
    return MICROLISP_OK;
}

const char *ml_sym_name(const ml_state *s, mvalue sym, size_t *out_len) {
    uint32_t idx = ml_sym_index(sym);
    if (idx >= s->symtab.count) {
        if (out_len != NULL) {
            *out_len = 0;
        }
        return "";
    }
    if (out_len != NULL) {
        *out_len = s->symtab.entries[idx].len;
    }
    return s->symtab.entries[idx].name;
}
