/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * Environment frames.
 *
 * Each frame holds a parallel pair of arrays (names, values) plus a
 * parent pointer. Lookup walks the chain frame-by-frame, scanning
 * each frame's names array linearly. For typical Scheme programs
 * frames are small (a handful of bindings) and the constant factor
 * beats a hash table.
 *
 * Both arrays are owned by the env and freed during sweep; the GC
 * marks every value at mark time so set!-extended bindings stay
 * alive.
 *
 * Note: this file does *not* allocate the env struct itself -- that's
 * ml_gc_alloc_env in gc.c. ml_env_define is responsible only for
 * growing the names/values arrays inside an already-allocated env.
 */

#include "microlisp_internal.h"

#include <stdlib.h>
#include <string.h>

static microlisp_status env_grow(ml_state *s, menv *e) {
    size_t new_cap = e->capacity == 0 ? 8 : e->capacity * 2;
    size_t bytes;
    if (__builtin_mul_overflow(new_cap, sizeof(mvalue), &bytes)) {
        return MICROLISP_ERR_NOMEM;
    }
    mvalue *new_names = (mvalue *)ml_raw_alloc(s, bytes);
    if (new_names == NULL) {
        return MICROLISP_ERR_NOMEM;
    }
    mvalue *new_values = (mvalue *)ml_raw_alloc(s, bytes);
    if (new_values == NULL) {
        ml_raw_free(s, new_names);
        return MICROLISP_ERR_NOMEM;
    }
    if (e->count > 0) {
        memcpy(new_names, e->names, e->count * sizeof(mvalue));
        memcpy(new_values, e->values, e->count * sizeof(mvalue));
    }
    ml_raw_free(s, e->names);
    ml_raw_free(s, e->values);
    e->names = new_names;
    e->values = new_values;
    e->capacity = new_cap;
    return MICROLISP_OK;
}

microlisp_status ml_env_define(ml_state *s, mvalue env_v, mvalue name, mvalue value) {
    if (!ml_is_env(env_v) || !ml_is_sym(name)) {
        return MICROLISP_ERR_INVALID_ARG;
    }
    menv *e = ml_as_env(env_v);

    /* Replace if already present in this exact frame. (R5RS says a
     * second top-level `define` of the same name is implementation-
     * defined; replacing is the friendly choice and matches what
     * most embeds do.) */
    for (size_t i = 0; i < e->count; i++) {
        if (e->names[i] == name) {
            e->values[i] = value;
            return MICROLISP_OK;
        }
    }

    if (e->count == e->capacity) {
        microlisp_status st = env_grow(s, e);
        if (st != MICROLISP_OK) {
            return st;
        }
    }
    e->names[e->count] = name;
    e->values[e->count] = value;
    e->count++;
    return MICROLISP_OK;
}

microlisp_status ml_env_set(ml_state *s, mvalue env_v, mvalue name, mvalue value) {
    (void)s;
    if (!ml_is_sym(name)) {
        return MICROLISP_ERR_INVALID_ARG;
    }
    mvalue cur = env_v;
    while (ml_is_env(cur)) {
        menv *e = ml_as_env(cur);
        for (size_t i = 0; i < e->count; i++) {
            if (e->names[i] == name) {
                e->values[i] = value;
                return MICROLISP_OK;
            }
        }
        cur = e->parent;
    }
    return MICROLISP_ERR_UNBOUND;
}

microlisp_status ml_env_lookup(const ml_state *s, mvalue env_v, mvalue name, mvalue *out) {
    (void)s;
    if (!ml_is_sym(name)) {
        return MICROLISP_ERR_INVALID_ARG;
    }
    mvalue cur = env_v;
    while (ml_is_env(cur)) {
        menv *e = ml_as_env(cur);
        for (size_t i = 0; i < e->count; i++) {
            if (e->names[i] == name) {
                *out = e->values[i];
                return MICROLISP_OK;
            }
        }
        cur = e->parent;
    }
    return MICROLISP_ERR_UNBOUND;
}
