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

#include <assert.h>
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
    /* Every heap object's pointer must have the low 3 bits clear, since
     * tagged values pack the type code there. state_create checks the
     * very first allocation; this assert catches a misbehaving arena
     * that aligns its first block correctly but later returns
     * weaker-aligned ones (a pre-mortem risk noted by the v0.1.4 cold
     * review). Compiled out in Release; an embedder whose arena
     * violates the contract sees the failure under any sanitizer or
     * debug build. */
    assert(((uintptr_t)o & (uintptr_t)0x7U) == 0U &&
           "microlisp_allocator returned a misaligned block; alloc must "
           "return pointers aligned to >= 8 bytes (see microlisp.h)");
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
 * Iterative BFS over the live graph using an explicit worklist on
 * the state. The recursive version overflowed the C stack on any
 * input that built a long flat list (`(loop n (cons n acc))`) or
 * a deeply-nested tree -- both shapes the fuzzer produces routinely
 * from tail-recursive accumulator programs.
 *
 * Allocation discipline: the worklist itself is heap-allocated via
 * the user's allocator. If a grow fails mid-collection, we abort
 * the entire mark+sweep cycle and leave the heap untouched -- worse
 * than collecting (we'll likely OOM soon if we couldn't grow), but
 * strictly safer than half-marking and freeing-as-unreachable an
 * object whose descendants we didn't get to visit.
 * -------------------------------------------------------------------------- */

/* Push @p o onto the mark worklist. Returns 1 on success, 0 on
 * allocation failure (in which case the caller must abort the
 * collection). The object is assumed not yet marked; the caller is
 * responsible for setting o->marked = 1 before pushing. */
static int gc_push_mark(ml_state *s, mobj *o) {
    if (s->gc_marklist_count == s->gc_marklist_cap) {
        size_t new_cap = s->gc_marklist_cap == 0 ? 64 : s->gc_marklist_cap * 2;
        size_t bytes;
        if (__builtin_mul_overflow(new_cap, sizeof(mobj *), &bytes)) {
            return 0;
        }
        mobj **grown = (mobj **)ml_raw_alloc(s, bytes);
        if (grown == NULL) {
            return 0;
        }
        if (s->gc_marklist != NULL) {
            memcpy((void *)grown, (const void *)s->gc_marklist,
                   s->gc_marklist_count * sizeof(mobj *));
            ml_raw_free(s, (void *)s->gc_marklist);
        }
        s->gc_marklist = grown;
        s->gc_marklist_cap = new_cap;
    }
    s->gc_marklist[s->gc_marklist_count++] = o;
    return 1;
}

/* If @p v points at an unmarked heap object, mark it and push it on
 * the worklist for later child-walking. Returns 1 on success, 0 on
 * grow-failure. */
static int gc_visit(ml_state *s, mvalue v) {
    if (M_TAG_OF(v) != M_TAG_OBJ || v == 0) {
        return 1;
    }
    mobj *o = ml_as_obj(v);
    if (o->marked) {
        return 1;
    }
    o->marked = 1;
    return gc_push_mark(s, o);
}

/* Drain the worklist, visiting each object's outgoing references.
 * Returns 1 on clean drain, 0 on OOM. */
static int gc_drain_worklist(ml_state *s) {
    while (s->gc_marklist_count > 0) {
        mobj *o = s->gc_marklist[--s->gc_marklist_count];
        switch (o->type) {
        case OBJ_PAIR: {
            mpair *p = (mpair *)o;
            if (!gc_visit(s, p->car) || !gc_visit(s, p->cdr)) {
                return 0;
            }
            break;
        }
        case OBJ_STRING:
        case OBJ_PRIMITIVE:
            /* No outgoing references. */
            break;
        case OBJ_CLOSURE: {
            mclosure *c = (mclosure *)o;
            if (!gc_visit(s, c->env) || !gc_visit(s, c->params) || !gc_visit(s, c->body)) {
                return 0;
            }
            break;
        }
        case OBJ_ENV: {
            menv *e = (menv *)o;
            if (!gc_visit(s, e->parent)) {
                return 0;
            }
            for (size_t i = 0; i < e->count; i++) {
                if (!gc_visit(s, e->values[i]) || !gc_visit(s, e->names[i])) {
                    return 0;
                }
            }
            break;
        }
        default:
            /* Unknown type -- skip; it's marked, won't be re-visited. */
            break;
        }
    }
    return 1;
}

/* Best-effort fallback when the worklist couldn't grow mid-collection:
 * clear every object's mark so a subsequent (successful) collection
 * starts fresh. Without this we'd half-mark the heap and the next
 * sweep would free reachable objects. */
static void gc_clear_all_marks(ml_state *s) {
    for (mobj *o = s->gc_head; o != NULL; o = o->next) {
        o->marked = 0;
    }
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
     * but gc_visit handles all tags uniformly. */
    s->gc_marklist_count = 0;
    int ok = gc_visit(s, s->toplevel_env);
    for (size_t i = 0; ok && i < s->gc_protect_count; i++) {
        ok = gc_visit(s, s->gc_protect[i]);
    }
    if (ok) {
        ok = gc_drain_worklist(s);
    }
    if (!ok) {
        /* Couldn't grow the worklist. Reset everything and bail; the
         * heap is unchanged. The next allocation that retries will
         * try again, by which point the caller may have freed memory. */
        gc_clear_all_marks(s);
        s->gc_marklist_count = 0;
        return;
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
