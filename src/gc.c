/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * Mark-and-sweep garbage collector and heap-object allocators.
 *
 * Every heap object is linked into ml_state::gc_head at construction.
 * A collection walks the roots -- the top-level environment plus
 * everything on the protect stack -- recursively marks reachable
 * objects, then sweeps the all-objects list and frees the unmarked.
 *
 * Collection trigger: a soft watermark on live-object count. After
 * each sweep the threshold is set to max(live * 2, initial), so a
 * program with a steady-state heap of N objects collects roughly
 * every N allocations.
 *
 * The protect stack is the discipline that keeps fresh allocations
 * alive. Any function that holds a heap-object handle in a C local
 * across a call that can allocate must protect it first; the eval
 * loop, reader, and printer all use ml_gc_savepoint / ml_gc_restore
 * scope guards.
 */

#include "microlisp_internal.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Raw-memory allocator wrapper.
 * -------------------------------------------------------------------------- */

void *ml_raw_alloc(ml_state *s, size_t size) {
    if (size == 0) {
        return NULL;
    }
    return s->allocator.alloc(size, s->allocator.user);
}

void ml_raw_free(ml_state *s, void *p) {
    if (p == NULL) {
        return;
    }
    s->allocator.free(p, s->allocator.user);
}

/* --------------------------------------------------------------------------
 * Object allocation.
 *
 * Common path: maybe collect, then allocate raw, then link.
 * -------------------------------------------------------------------------- */

static void maybe_collect(ml_state *s) {
    if (s->gc_stress || s->gc_live_objects >= s->gc_threshold) {
        ml_gc_collect(s);
    }
}

static void link_object(ml_state *s, mobj *o) {
    o->next = s->gc_head;
    s->gc_head = o;
    s->gc_live_objects++;
}

microlisp_status ml_gc_alloc_pair(ml_state *s, mvalue car, mvalue cdr, mvalue *out) {
    maybe_collect(s);
    mpair *p = (mpair *)ml_raw_alloc(s, sizeof(mpair));
    if (p == NULL) {
        return MICROLISP_ERR_NOMEM;
    }
    p->h.type = OBJ_PAIR;
    p->h.marked = 0;
    p->car = car;
    p->cdr = cdr;
    link_object(s, &p->h);
    *out = (mvalue)(uintptr_t)p;
    return MICROLISP_OK;
}

microlisp_status ml_gc_alloc_string(ml_state *s, const uint8_t *bytes, size_t len, mvalue *out) {
    maybe_collect(s);
    /* sizeof(mstring) already counts the (zero-length) flexible array as 0;
     * add `len` for the payload. Watch the +0 case so `bytes` may be NULL. */
    size_t total;
    if (__builtin_add_overflow(sizeof(mstring), len, &total)) {
        return MICROLISP_ERR_NOMEM;
    }
    mstring *str = (mstring *)ml_raw_alloc(s, total);
    if (str == NULL) {
        return MICROLISP_ERR_NOMEM;
    }
    str->h.type = OBJ_STRING;
    str->h.marked = 0;
    str->len = len;
    if (len > 0 && bytes != NULL) {
        memcpy(str->bytes, bytes, len);
    }
    link_object(s, &str->h);
    *out = (mvalue)(uintptr_t)str;
    return MICROLISP_OK;
}

microlisp_status ml_gc_alloc_closure(ml_state *s, mvalue env, mvalue params, mvalue body,
                                     mvalue *out) {
    maybe_collect(s);
    mclosure *c = (mclosure *)ml_raw_alloc(s, sizeof(mclosure));
    if (c == NULL) {
        return MICROLISP_ERR_NOMEM;
    }
    c->h.type = OBJ_CLOSURE;
    c->h.marked = 0;
    c->env = env;
    c->params = params;
    c->body = body;
    c->variadic = 0;
    c->arity_min = 0;
    link_object(s, &c->h);
    *out = (mvalue)(uintptr_t)c;
    return MICROLISP_OK;
}

microlisp_status ml_gc_alloc_primitive(ml_state *s, const char *name, ml_prim_fn fn,
                                       uint32_t arity_min, uint32_t arity_max, mvalue *out) {
    maybe_collect(s);
    mprim *p = (mprim *)ml_raw_alloc(s, sizeof(mprim));
    if (p == NULL) {
        return MICROLISP_ERR_NOMEM;
    }
    p->h.type = OBJ_PRIMITIVE;
    p->h.marked = 0;
    p->name = name;
    p->fn = fn;
    p->arity_min = arity_min;
    p->arity_max = arity_max;
    link_object(s, &p->h);
    *out = (mvalue)(uintptr_t)p;
    return MICROLISP_OK;
}

microlisp_status ml_gc_alloc_env(ml_state *s, mvalue parent, mvalue *out) {
    maybe_collect(s);
    menv *e = (menv *)ml_raw_alloc(s, sizeof(menv));
    if (e == NULL) {
        return MICROLISP_ERR_NOMEM;
    }
    e->h.type = OBJ_ENV;
    e->h.marked = 0;
    e->parent = parent;
    e->count = 0;
    e->capacity = 0;
    e->names = NULL;
    e->values = NULL;
    link_object(s, &e->h);
    *out = (mvalue)(uintptr_t)e;
    return MICROLISP_OK;
}

/* --------------------------------------------------------------------------
 * Protect stack.
 * -------------------------------------------------------------------------- */

microlisp_status ml_gc_protect(ml_state *s, mvalue v) {
    if (s->gc_protect_count == s->gc_protect_cap) {
        size_t new_cap = s->gc_protect_cap == 0 ? 32 : s->gc_protect_cap * 2;
        size_t bytes;
        if (__builtin_mul_overflow(new_cap, sizeof(mvalue), &bytes)) {
            return MICROLISP_ERR_NOMEM;
        }
        mvalue *grown = (mvalue *)ml_raw_alloc(s, bytes);
        if (grown == NULL) {
            return MICROLISP_ERR_NOMEM;
        }
        if (s->gc_protect != NULL) {
            memcpy(grown, s->gc_protect, s->gc_protect_count * sizeof(mvalue));
            ml_raw_free(s, s->gc_protect);
        }
        s->gc_protect = grown;
        s->gc_protect_cap = new_cap;
    }
    s->gc_protect[s->gc_protect_count++] = v;
    return MICROLISP_OK;
}

void ml_gc_unprotect(ml_state *s, size_t n) {
    if (n > s->gc_protect_count) {
        n = s->gc_protect_count;
    }
    s->gc_protect_count -= n;
}

void ml_gc_restore(ml_state *s, size_t saved) {
    if (saved < s->gc_protect_count) {
        s->gc_protect_count = saved;
    }
}

/* --------------------------------------------------------------------------
 * Mark.
 *
 * Recursive walk over the live graph. Recursion is bounded by the
 * reader's depth limit (default 256) for parser-built structures, and
 * by user-controlled depth for builder-built ones. A user program
 * that constructs a deeply nested list via repeated `cons` can in
 * principle blow the C stack inside mark(); document this and treat
 * it as a v0.2 concern (iterative marker with an explicit stack).
 * -------------------------------------------------------------------------- */

static void mark_value(mvalue v);

static void mark_obj(mobj *o) {
    if (o == NULL || o->marked) {
        return;
    }
    o->marked = 1;
    switch (o->type) {
    case OBJ_PAIR: {
        mpair *p = (mpair *)o;
        mark_value(p->car);
        mark_value(p->cdr);
        break;
    }
    case OBJ_STRING:
        /* No outgoing references. */
        break;
    case OBJ_CLOSURE: {
        mclosure *c = (mclosure *)o;
        mark_value(c->env);
        mark_value(c->params);
        mark_value(c->body);
        break;
    }
    case OBJ_PRIMITIVE:
        /* No outgoing references. */
        break;
    case OBJ_ENV: {
        menv *e = (menv *)o;
        mark_value(e->parent);
        for (size_t i = 0; i < e->count; i++) {
            mark_value(e->values[i]);
            /* names[i] is a symbol value -- tagged, no heap pointer to
             * follow, but mark_value handles all tags uniformly. */
            mark_value(e->names[i]);
        }
        break;
    }
    default:
        /* Unknown type -- shouldn't happen; leave the mark in place so
         * we don't loop on a cycle, but the object will also not be
         * recursively walked, which surfaces any unreferenced child
         * via a use-after-free under sanitizers. */
        break;
    }
}

static void mark_value(mvalue v) {
    if (M_TAG_OF(v) != M_TAG_OBJ) {
        return; /* immediate / fixnum / symbol: no heap object behind it. */
    }
    if (v == 0) {
        return;
    }
    mark_obj(ml_as_obj(v));
}

/* --------------------------------------------------------------------------
 * Sweep.
 * -------------------------------------------------------------------------- */

static void free_obj(ml_state *s, mobj *o) {
    if (o->type == OBJ_ENV) {
        menv *e = (menv *)o;
        ml_raw_free(s, e->names);
        ml_raw_free(s, e->values);
    }
    ml_raw_free(s, o);
}

static void sweep(ml_state *s) {
    mobj **link = &s->gc_head;
    size_t live = 0;
    while (*link != NULL) {
        mobj *o = *link;
        if (o->marked) {
            o->marked = 0;
            link = &o->next;
            live++;
        } else {
            *link = o->next;
            free_obj(s, o);
        }
    }
    s->gc_live_objects = live;
}

void ml_gc_collect(ml_state *s) {
    /* Roots: top-level env + protect stack + pre-interned-symbol mvalues.
     * The interned symbols are not heap objects (they're tagged-immediate),
     * but marking them is a no-op so the pattern stays uniform. */
    mark_value(s->toplevel_env);
    for (size_t i = 0; i < s->gc_protect_count; i++) {
        mark_value(s->gc_protect[i]);
    }
    sweep(s);

    /* Recompute the threshold so steady-state programs collect about
     * once per generation of live objects. The lower bound prevents
     * thrashing on a near-empty heap. */
    size_t doubled;
    if (__builtin_mul_overflow(s->gc_live_objects, (size_t)2, &doubled)) {
        doubled = SIZE_MAX;
    }
    size_t floor = 4096;
    s->gc_threshold = doubled > floor ? doubled : floor;
}

/* --------------------------------------------------------------------------
 * Destroy-time bulk free.
 *
 * No marking; everything goes.
 * -------------------------------------------------------------------------- */

void ml_gc_free_all(ml_state *s) {
    mobj *o = s->gc_head;
    while (o != NULL) {
        mobj *next = o->next;
        free_obj(s, o);
        o = next;
    }
    s->gc_head = NULL;
    s->gc_live_objects = 0;
}
