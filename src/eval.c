/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * Trampolined evaluator with proper tail-call optimization.
 *
 * The main loop runs `form` against `env` and either returns a value,
 * propagates an error, or updates `(form, env)` and loops. Special
 * forms whose continuation is a single tail expression -- `if`,
 * `begin`, `let`, `let*`, `letrec`, `and`, `or`, `cond`, and the
 * closure call itself -- all set up the next iteration and `continue`
 * instead of recursing. The result is O(1) C-stack growth for any
 * tail-recursive Scheme program.
 *
 * GC discipline inside the loop:
 *
 *   - `form` and `env` are kept on the protect stack at fixed offsets
 *     for the entire lifetime of one eval invocation. They're updated
 *     in place when a TCO continuation reassigns them.
 *   - Helpers that allocate (e.g., closure application, argument
 *     collection) take a savepoint, push the values they need to
 *     survive a possible collection, and restore before returning to
 *     the loop.
 */

#include "microlisp_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * List helpers.
 * -------------------------------------------------------------------------- */

static int proper_list_length(mvalue v, size_t *out_len) {
    size_t n = 0;
    while (ml_is_pair(v)) {
        n++;
        v = ml_as_pair(v)->cdr;
    }
    if (v != MV_NIL) {
        return 0; /* improper */
    }
    *out_len = n;
    return 1;
}

/* Eval each element of a proper list, returning a freshly allocated
 * array of values. The array is owned by the caller (free with
 * ml_raw_free). Each element value is pushed onto the protect stack
 * starting at `s->gc_protect_count` so they survive subsequent
 * allocations; the caller restores via ml_gc_restore. */
static microlisp_status eval_arg_list(ml_state *s, mvalue args, mvalue env, mvalue **out_argv,
                                      size_t *out_argc) {
    size_t argc = 0;
    if (!proper_list_length(args, &argc)) {
        ml_set_error(s, 0, 0, "argument list is not a proper list");
        return MICROLISP_ERR_SYNTAX;
    }
    if (argc == 0) {
        *out_argv = NULL;
        *out_argc = 0;
        return MICROLISP_OK;
    }
    size_t bytes;
    if (__builtin_mul_overflow(argc, sizeof(mvalue), &bytes)) {
        return MICROLISP_ERR_NOMEM;
    }
    mvalue *argv = (mvalue *)ml_raw_alloc(s, bytes);
    if (argv == NULL) {
        return MICROLISP_ERR_NOMEM;
    }
    /* Initialize so the protect-stack values are well-defined before
     * we eval anything. */
    for (size_t i = 0; i < argc; i++) {
        argv[i] = MV_UNDEF;
    }
    /* Hand-off note: argv isn't on the heap or the protect stack
     * directly; we push each *value* we evaluate onto the protect
     * stack instead. The array itself just collects the protected
     * values for the caller. */
    size_t pushed = 0;
    mvalue cur = args;
    for (size_t i = 0; i < argc; i++) {
        mvalue elem = ml_as_pair(cur)->car;
        mvalue v;
        microlisp_status st = ml_eval(s, elem, env, &v);
        if (st != MICROLISP_OK) {
            ml_gc_unprotect(s, pushed);
            ml_raw_free(s, argv);
            return st;
        }
        st = ml_gc_protect(s, v);
        if (st != MICROLISP_OK) {
            ml_gc_unprotect(s, pushed);
            ml_raw_free(s, argv);
            return st;
        }
        pushed++;
        argv[i] = v;
        cur = ml_as_pair(cur)->cdr;
    }
    *out_argv = argv;
    *out_argc = argc;
    return MICROLISP_OK;
    /* Caller is responsible for ml_gc_unprotect(s, *out_argc) once the
     * args are no longer needed (i.e., after binding them into an env
     * or invoking the primitive). */
}

/* Build a Scheme list from argv[start..argc). Used for variadic
 * rest-args. Items in argv must be on the protect stack or otherwise
 * reachable. */
static microlisp_status build_list(ml_state *s, const mvalue *argv, size_t start, size_t end,
                                   mvalue *out) {
    mvalue head = MV_NIL;
    size_t sp = ml_gc_savepoint(s);
    microlisp_status st = ml_gc_protect(s, head);
    if (st != MICROLISP_OK) {
        return st;
    }
    /* Iterate in reverse so each cons gets the partial-tail we've
     * already protected. */
    for (size_t i = end; i > start; i--) {
        mvalue elem = argv[i - 1];
        st = ml_gc_protect(s, elem);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        mvalue cell;
        st = ml_gc_alloc_pair(s, elem, head, &cell);
        ml_gc_unprotect(s, 1); /* elem */
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        head = cell;
        s->gc_protect[sp] = head; /* refresh protected head slot */
    }
    *out = head;
    ml_gc_restore(s, sp);
    return MICROLISP_OK;
}

/* --------------------------------------------------------------------------
 * Closure construction (used by lambda) and application.
 * -------------------------------------------------------------------------- */

static microlisp_status closure_arity(ml_state *s, mvalue params, uint32_t *out_min,
                                      int *out_variadic) {
    uint32_t n = 0;
    mvalue cur = params;
    while (ml_is_pair(cur)) {
        if (!ml_is_sym(ml_as_pair(cur)->car)) {
            ml_set_error(s, 0, 0, "lambda parameter must be a symbol");
            return MICROLISP_ERR_SYNTAX;
        }
        n++;
        cur = ml_as_pair(cur)->cdr;
    }
    if (cur == MV_NIL) {
        *out_min = n;
        *out_variadic = 0;
        return MICROLISP_OK;
    }
    if (ml_is_sym(cur)) {
        *out_min = n;
        *out_variadic = 1;
        return MICROLISP_OK;
    }
    ml_set_error(s, 0, 0, "lambda parameter list is malformed");
    return MICROLISP_ERR_SYNTAX;
}

static microlisp_status make_closure(ml_state *s, mvalue params, mvalue body, mvalue env,
                                     mvalue *out) {
    uint32_t arity_min = 0;
    int variadic = 0;
    microlisp_status st = closure_arity(s, params, &arity_min, &variadic);
    if (st != MICROLISP_OK) {
        return st;
    }
    /* Protect inputs across allocation. */
    size_t sp = ml_gc_savepoint(s);
    if ((st = ml_gc_protect(s, params)) != MICROLISP_OK ||
        (st = ml_gc_protect(s, body)) != MICROLISP_OK ||
        (st = ml_gc_protect(s, env)) != MICROLISP_OK) {
        ml_gc_restore(s, sp);
        return st;
    }
    mvalue cl;
    st = ml_gc_alloc_closure(s, env, params, body, &cl);
    ml_gc_restore(s, sp);
    if (st != MICROLISP_OK) {
        return st;
    }
    ml_as_closure(cl)->arity_min = arity_min;
    ml_as_closure(cl)->variadic = variadic;
    *out = cl;
    return MICROLISP_OK;
}

/* Bind argv into a fresh environment that extends `parent`. */
static microlisp_status apply_closure_bind(ml_state *s, mclosure *cl, size_t argc,
                                           const mvalue *argv, mvalue *out_env) {
    if (cl->variadic) {
        if (argc < cl->arity_min) {
            ml_set_error(s, 0, 0, "expected at least %u arguments, got %zu", cl->arity_min, argc);
            return MICROLISP_ERR_ARITY;
        }
    } else {
        if (argc != cl->arity_min) {
            ml_set_error(s, 0, 0, "expected %u arguments, got %zu", cl->arity_min, argc);
            return MICROLISP_ERR_ARITY;
        }
    }

    mvalue env;
    microlisp_status st = ml_gc_alloc_env(s, cl->env, &env);
    if (st != MICROLISP_OK) {
        return st;
    }
    size_t sp = ml_gc_savepoint(s);
    st = ml_gc_protect(s, env);
    if (st != MICROLISP_OK) {
        return st;
    }

    /* Bind the named (positional) parameters first. */
    mvalue cur = cl->params;
    for (size_t i = 0; i < cl->arity_min; i++) {
        mvalue name = ml_as_pair(cur)->car;
        st = ml_env_define(s, env, name, argv[i]);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        cur = ml_as_pair(cur)->cdr;
    }

    /* Variadic rest: cur is either MV_NIL (no rest) or a symbol. */
    if (cl->variadic) {
        /* closure_arity already verified the structure; this is a
         * belt-and-braces check for use-after-mutation of params. */
        if (!ml_is_sym(cur)) {
            ml_set_error(s, 0, 0, "malformed variadic parameter list");
            ml_gc_restore(s, sp);
            return MICROLISP_ERR_SYNTAX;
        }
        mvalue rest_name = cur;
        mvalue rest_list;
        st = build_list(s, argv, cl->arity_min, argc, &rest_list);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        st = ml_env_define(s, env, rest_name, rest_list);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
    }

    *out_env = env;
    ml_gc_restore(s, sp);
    return MICROLISP_OK;
}

/* --------------------------------------------------------------------------
 * Special-form handlers. Each returns either:
 *   - OK, set *out and signal "non-TCO" via *did_tco = 0; or
 *   - OK, set *new_form and *new_env and signal *did_tco = 1; or
 *   - an error status.
 * -------------------------------------------------------------------------- */

typedef enum {
    SF_NOT_HANDLED = 0,
    SF_VALUE, /* result is in *out */
    SF_TCO    /* continue with new_form / new_env */
} sf_result;

/* Handle one special form. Returns SF_NOT_HANDLED to indicate the
 * head wasn't a special-form symbol -- caller should treat as a
 * regular procedure call. */
static microlisp_status handle_quote(ml_state *s, mvalue args, mvalue *out, sf_result *result) {
    size_t n;
    if (!proper_list_length(args, &n) || n != 1) {
        ml_set_error(s, 0, 0, "quote requires exactly one argument");
        return MICROLISP_ERR_SYNTAX;
    }
    *out = ml_as_pair(args)->car;
    *result = SF_VALUE;
    return MICROLISP_OK;
}

static microlisp_status handle_if(ml_state *s, mvalue args, mvalue env, mvalue *new_form,
                                  mvalue *new_env, mvalue *out, sf_result *result) {
    size_t n;
    if (!proper_list_length(args, &n) || (n != 2 && n != 3)) {
        ml_set_error(s, 0, 0, "if requires 2 or 3 arguments");
        return MICROLISP_ERR_SYNTAX;
    }
    mvalue test_form = ml_as_pair(args)->car;
    mvalue rest = ml_as_pair(args)->cdr;
    mvalue then_form = ml_as_pair(rest)->car;
    mvalue else_form = (n == 3) ? ml_as_pair(ml_as_pair(rest)->cdr)->car : MV_UNDEF;

    mvalue test_v;
    microlisp_status st = ml_eval(s, test_form, env, &test_v);
    if (st != MICROLISP_OK) {
        return st;
    }
    if (ml_truthy(test_v)) {
        *new_form = then_form;
    } else if (n == 3) {
        *new_form = else_form;
    } else {
        *out = MV_UNDEF;
        *result = SF_VALUE;
        return MICROLISP_OK;
    }
    *new_env = env;
    *result = SF_TCO;
    return MICROLISP_OK;
}

static microlisp_status handle_define(ml_state *s, mvalue args, mvalue env, mvalue *out,
                                      sf_result *result) {
    /* Two forms:
     *   (define <symbol> <expr>)
     *   (define (<symbol> <params...>) <body>...)   sugar for lambda
     */
    size_t n;
    if (!proper_list_length(args, &n) || n < 2) {
        ml_set_error(s, 0, 0, "define requires a name and a body");
        return MICROLISP_ERR_SYNTAX;
    }
    mvalue first = ml_as_pair(args)->car;
    mvalue rest = ml_as_pair(args)->cdr;
    if (ml_is_sym(first)) {
        if (n != 2) {
            ml_set_error(s, 0, 0, "define with a name takes exactly one value expression");
            return MICROLISP_ERR_SYNTAX;
        }
        mvalue value;
        microlisp_status st = ml_eval(s, ml_as_pair(rest)->car, env, &value);
        if (st != MICROLISP_OK) {
            return st;
        }
        size_t sp = ml_gc_savepoint(s);
        st = ml_gc_protect(s, value);
        if (st != MICROLISP_OK) {
            return st;
        }
        st = ml_env_define(s, env, first, value);
        ml_gc_restore(s, sp);
        if (st != MICROLISP_OK) {
            return st;
        }
        *out = MV_UNDEF;
        *result = SF_VALUE;
        return MICROLISP_OK;
    }
    if (ml_is_pair(first)) {
        mvalue name = ml_as_pair(first)->car;
        if (!ml_is_sym(name)) {
            ml_set_error(s, 0, 0, "define: name must be a symbol");
            return MICROLISP_ERR_SYNTAX;
        }
        mvalue params = ml_as_pair(first)->cdr;
        mvalue body = rest; /* one or more body forms */
        mvalue cl;
        microlisp_status st = make_closure(s, params, body, env, &cl);
        if (st != MICROLISP_OK) {
            return st;
        }
        size_t sp = ml_gc_savepoint(s);
        st = ml_gc_protect(s, cl);
        if (st != MICROLISP_OK) {
            return st;
        }
        st = ml_env_define(s, env, name, cl);
        ml_gc_restore(s, sp);
        if (st != MICROLISP_OK) {
            return st;
        }
        *out = MV_UNDEF;
        *result = SF_VALUE;
        return MICROLISP_OK;
    }
    ml_set_error(s, 0, 0, "define: first argument must be a symbol or list");
    return MICROLISP_ERR_SYNTAX;
}

static microlisp_status handle_set_bang(ml_state *s, mvalue args, mvalue env, mvalue *out,
                                        sf_result *result) {
    size_t n;
    if (!proper_list_length(args, &n) || n != 2) {
        ml_set_error(s, 0, 0, "set! requires a name and a value");
        return MICROLISP_ERR_SYNTAX;
    }
    mvalue name = ml_as_pair(args)->car;
    if (!ml_is_sym(name)) {
        ml_set_error(s, 0, 0, "set!: target must be a symbol");
        return MICROLISP_ERR_SYNTAX;
    }
    mvalue value;
    microlisp_status st = ml_eval(s, ml_as_pair(ml_as_pair(args)->cdr)->car, env, &value);
    if (st != MICROLISP_OK) {
        return st;
    }
    size_t sp = ml_gc_savepoint(s);
    st = ml_gc_protect(s, value);
    if (st != MICROLISP_OK) {
        return st;
    }
    st = ml_env_set(s, env, name, value);
    ml_gc_restore(s, sp);
    if (st == MICROLISP_ERR_UNBOUND) {
        size_t nlen;
        const char *nm = ml_sym_name(s, name, &nlen);
        ml_set_error(s, 0, 0, "set!: unbound variable `%.*s`", (int)nlen, nm);
        return st;
    }
    if (st != MICROLISP_OK) {
        return st;
    }
    *out = MV_UNDEF;
    *result = SF_VALUE;
    return MICROLISP_OK;
}

static microlisp_status handle_lambda(ml_state *s, mvalue args, mvalue env, mvalue *out,
                                      sf_result *result) {
    size_t n;
    if (!proper_list_length(args, &n) || n < 2) {
        ml_set_error(s, 0, 0, "lambda requires a parameter list and a body");
        return MICROLISP_ERR_SYNTAX;
    }
    mvalue params = ml_as_pair(args)->car;
    mvalue body = ml_as_pair(args)->cdr;
    if (!ml_is_pair(params) && params != MV_NIL && !ml_is_sym(params)) {
        ml_set_error(s, 0, 0, "lambda: parameter spec must be a list or symbol");
        return MICROLISP_ERR_SYNTAX;
    }
    mvalue cl;
    microlisp_status st = make_closure(s, params, body, env, &cl);
    if (st != MICROLISP_OK) {
        return st;
    }
    *out = cl;
    *result = SF_VALUE;
    return MICROLISP_OK;
}

/* `begin`: evaluate every form for effect, leave the last for TCO. */
static microlisp_status begin_tco(ml_state *s, mvalue forms, mvalue env, mvalue *new_form,
                                  mvalue *new_env, mvalue *out, sf_result *result) {
    if (forms == MV_NIL) {
        *out = MV_UNDEF;
        *result = SF_VALUE;
        return MICROLISP_OK;
    }
    /* Reject improper lists up front so the loop below can dereference
     * pairs safely. Without this check, `(begin . 1)` or `(cond (#t . 1))`
     * casts a non-pair tail to a pair pointer and crashes. */
    size_t form_count;
    if (!proper_list_length(forms, &form_count)) {
        ml_set_error(s, 0, 0, "body must be a proper list");
        return MICROLISP_ERR_SYNTAX;
    }
    while (ml_is_pair(ml_as_pair(forms)->cdr)) {
        mvalue ignored;
        microlisp_status st = ml_eval(s, ml_as_pair(forms)->car, env, &ignored);
        if (st != MICROLISP_OK) {
            return st;
        }
        forms = ml_as_pair(forms)->cdr;
    }
    *new_form = ml_as_pair(forms)->car;
    *new_env = env;
    *result = SF_TCO;
    return MICROLISP_OK;
}

static microlisp_status handle_begin(ml_state *s, mvalue args, mvalue env, mvalue *new_form,
                                     mvalue *new_env, mvalue *out, sf_result *result) {
    return begin_tco(s, args, env, new_form, new_env, out, result);
}

/* let / let* / letrec all extend env then evaluate body via begin_tco. */
static microlisp_status handle_let_family(ml_state *s, mvalue args, mvalue env, int starlike,
                                          int recursive, mvalue *new_form, mvalue *new_env,
                                          mvalue *out, sf_result *result, const char *form_name) {
    size_t n;
    if (!proper_list_length(args, &n) || n < 2) {
        ml_set_error(s, 0, 0, "%s requires a bindings list and a body", form_name);
        return MICROLISP_ERR_SYNTAX;
    }
    mvalue bindings = ml_as_pair(args)->car;
    mvalue body = ml_as_pair(args)->cdr;
    size_t blen;
    if (!proper_list_length(bindings, &blen)) {
        ml_set_error(s, 0, 0, "%s: bindings must be a proper list", form_name);
        return MICROLISP_ERR_SYNTAX;
    }

    mvalue inner;
    microlisp_status st = ml_gc_alloc_env(s, env, &inner);
    if (st != MICROLISP_OK) {
        return st;
    }
    size_t sp = ml_gc_savepoint(s);
    st = ml_gc_protect(s, inner);
    if (st != MICROLISP_OK) {
        return st;
    }

    /* For letrec, pre-define all bindings to MV_UNDEF so each init
     * expression can reference (the still-undefined) others. */
    if (recursive) {
        mvalue cur = bindings;
        while (ml_is_pair(cur)) {
            mvalue pair = ml_as_pair(cur)->car;
            if (!ml_is_pair(pair) || !ml_is_pair(ml_as_pair(pair)->cdr) ||
                ml_as_pair(ml_as_pair(pair)->cdr)->cdr != MV_NIL ||
                !ml_is_sym(ml_as_pair(pair)->car)) {
                ml_set_error(s, 0, 0, "%s: bindings must look like (name expr)", form_name);
                ml_gc_restore(s, sp);
                return MICROLISP_ERR_SYNTAX;
            }
            st = ml_env_define(s, inner, ml_as_pair(pair)->car, MV_UNDEF);
            if (st != MICROLISP_OK) {
                ml_gc_restore(s, sp);
                return st;
            }
            cur = ml_as_pair(cur)->cdr;
        }
    }

    mvalue cur = bindings;
    while (ml_is_pair(cur)) {
        mvalue pair = ml_as_pair(cur)->car;
        /* A binding is exactly (name expr): pair, with cdr being a pair
         * whose cdr is nil, and a symbol in the name slot. Without the
         * cdr.cdr.nil check, (let ((x 1 2)) ...) is silently accepted. */
        if (!ml_is_pair(pair) || !ml_is_pair(ml_as_pair(pair)->cdr) ||
            ml_as_pair(ml_as_pair(pair)->cdr)->cdr != MV_NIL || !ml_is_sym(ml_as_pair(pair)->car)) {
            ml_set_error(s, 0, 0, "%s: bindings must look like (name expr)", form_name);
            ml_gc_restore(s, sp);
            return MICROLISP_ERR_SYNTAX;
        }
        mvalue name = ml_as_pair(pair)->car;
        mvalue init = ml_as_pair(ml_as_pair(pair)->cdr)->car;
        mvalue init_env = starlike ? inner : (recursive ? inner : env);
        mvalue v;
        st = ml_eval(s, init, init_env, &v);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        size_t sp2 = ml_gc_savepoint(s);
        st = ml_gc_protect(s, v);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        st = ml_env_define(s, inner, name, v);
        ml_gc_restore(s, sp2);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        cur = ml_as_pair(cur)->cdr;
    }

    st = begin_tco(s, body, inner, new_form, new_env, out, result);
    ml_gc_restore(s, sp);
    return st;
}

/* and / or short-circuit. The chain's last element runs in tail
 * position. */
static microlisp_status handle_and_or(ml_state *s, mvalue args, mvalue env, int is_or,
                                      mvalue *new_form, mvalue *new_env, mvalue *out,
                                      sf_result *result) {
    if (args == MV_NIL) {
        /* (and) -> #t,  (or) -> #f */
        *out = is_or ? MV_FALSE : MV_TRUE;
        *result = SF_VALUE;
        return MICROLISP_OK;
    }
    /* Reject improper lists: `(and . 1)` would otherwise cast a fixnum
     * tail to a pair pointer in the loop below. */
    size_t arg_count;
    if (!proper_list_length(args, &arg_count)) {
        ml_set_error(s, 0, 0, "%s: argument list must be a proper list", is_or ? "or" : "and");
        return MICROLISP_ERR_SYNTAX;
    }
    while (ml_is_pair(ml_as_pair(args)->cdr)) {
        mvalue v;
        microlisp_status st = ml_eval(s, ml_as_pair(args)->car, env, &v);
        if (st != MICROLISP_OK) {
            return st;
        }
        int truthy = ml_truthy(v);
        if (is_or ? truthy : !truthy) {
            *out = v;
            *result = SF_VALUE;
            return MICROLISP_OK;
        }
        args = ml_as_pair(args)->cdr;
    }
    *new_form = ml_as_pair(args)->car;
    *new_env = env;
    *result = SF_TCO;
    return MICROLISP_OK;
}

static microlisp_status handle_cond(ml_state *s, mvalue args, mvalue env, mvalue *new_form,
                                    mvalue *new_env, mvalue *out, sf_result *result) {
    size_t clause_count;
    if (!proper_list_length(args, &clause_count)) {
        ml_set_error(s, 0, 0, "cond: clause list must be a proper list");
        return MICROLISP_ERR_SYNTAX;
    }
    mvalue cur = args;
    while (ml_is_pair(cur)) {
        mvalue clause = ml_as_pair(cur)->car;
        if (!ml_is_pair(clause)) {
            ml_set_error(s, 0, 0, "cond: clause must be a list");
            return MICROLISP_ERR_SYNTAX;
        }
        mvalue test = ml_as_pair(clause)->car;
        mvalue body = ml_as_pair(clause)->cdr;
        if (test == s->sym_else) {
            return begin_tco(s, body, env, new_form, new_env, out, result);
        }
        mvalue test_v;
        microlisp_status st = ml_eval(s, test, env, &test_v);
        if (st != MICROLISP_OK) {
            return st;
        }
        if (ml_truthy(test_v)) {
            if (body == MV_NIL) {
                /* `(cond (test))` form: yield the test value. */
                *out = test_v;
                *result = SF_VALUE;
                return MICROLISP_OK;
            }
            return begin_tco(s, body, env, new_form, new_env, out, result);
        }
        cur = ml_as_pair(cur)->cdr;
    }
    *out = MV_UNDEF;
    *result = SF_VALUE;
    return MICROLISP_OK;
}

/* --------------------------------------------------------------------------
 * Eval loop.
 * -------------------------------------------------------------------------- */

/* Forward decl: actual eval body. ml_eval just wraps it to manage the
 * eval-depth counter cleanly. */
static microlisp_status eval_body(ml_state *s, mvalue form, mvalue env, mvalue *out);

microlisp_status ml_eval(ml_state *s, mvalue form, mvalue env, mvalue *out) {
    if (s->eval_depth >= s->max_eval_depth) {
        ml_set_error(s, 0, 0,
                     "eval recursion depth exceeded limit of %zu (non-tail-recursive code)",
                     s->max_eval_depth);
        return MICROLISP_ERR_EVAL_DEPTH;
    }
    s->eval_depth++;
    microlisp_status st = eval_body(s, form, env, out);
    s->eval_depth--;
    return st;
}

static microlisp_status eval_body(ml_state *s, mvalue form, mvalue env, mvalue *out) {
    size_t sp = ml_gc_savepoint(s);
    microlisp_status st = ml_gc_protect(s, form);
    if (st != MICROLISP_OK) {
        return st;
    }
    st = ml_gc_protect(s, env);
    if (st != MICROLISP_OK) {
        ml_gc_restore(s, sp);
        return st;
    }
    /* sp+0 holds the current form, sp+1 holds the current env -- we
     * mutate them in place on every TCO loop iteration so the
     * collector sees the up-to-date roots. */
    const size_t form_slot = sp;
    const size_t env_slot = sp + 1;

    for (;;) {
        form = s->gc_protect[form_slot];
        env = s->gc_protect[env_slot];

        /* Self-evaluating values. */
        switch (M_TAG_OF(form)) {
        case M_TAG_FIX:
        case M_TAG_IMM:
            *out = form;
            ml_gc_restore(s, sp);
            return MICROLISP_OK;
        case M_TAG_SYM: {
            mvalue v;
            st = ml_env_lookup(s, env, form, &v);
            if (st == MICROLISP_ERR_UNBOUND) {
                size_t nlen;
                const char *nm = ml_sym_name(s, form, &nlen);
                ml_set_error(s, 0, 0, "unbound variable `%.*s`", (int)nlen, nm);
                ml_gc_restore(s, sp);
                return st;
            }
            *out = v;
            ml_gc_restore(s, sp);
            return st;
        }
        case M_TAG_OBJ:
            break;
        default:
            ml_set_error(s, 0, 0, "evaluator saw a value with unknown tag");
            ml_gc_restore(s, sp);
            return MICROLISP_ERR_TYPE;
        }

        /* Heap-object: strings, closures, primitives self-evaluate; only
         * pairs are forms to evaluate further. */
        if (!ml_is_pair(form)) {
            *out = form;
            ml_gc_restore(s, sp);
            return MICROLISP_OK;
        }

        mvalue head = ml_as_pair(form)->car;
        mvalue rest = ml_as_pair(form)->cdr;

        /* Special-form dispatch by interned symbol identity. */
        if (ml_is_sym(head)) {
            sf_result result = SF_NOT_HANDLED;
            mvalue new_form = MV_UNDEF;
            mvalue new_env = MV_UNDEF;
            mvalue value = MV_UNDEF;

            if (head == s->sym_quote) {
                st = handle_quote(s, rest, &value, &result);
            } else if (head == s->sym_if) {
                st = handle_if(s, rest, env, &new_form, &new_env, &value, &result);
            } else if (head == s->sym_define) {
                st = handle_define(s, rest, env, &value, &result);
            } else if (head == s->sym_set_bang) {
                st = handle_set_bang(s, rest, env, &value, &result);
            } else if (head == s->sym_lambda) {
                st = handle_lambda(s, rest, env, &value, &result);
            } else if (head == s->sym_begin) {
                st = handle_begin(s, rest, env, &new_form, &new_env, &value, &result);
            } else if (head == s->sym_let) {
                st = handle_let_family(s, rest, env, /*starlike=*/0, /*recursive=*/0, &new_form,
                                       &new_env, &value, &result, "let");
            } else if (head == s->sym_let_star) {
                st = handle_let_family(s, rest, env, /*starlike=*/1, /*recursive=*/0, &new_form,
                                       &new_env, &value, &result, "let*");
            } else if (head == s->sym_letrec) {
                st = handle_let_family(s, rest, env, /*starlike=*/1, /*recursive=*/1, &new_form,
                                       &new_env, &value, &result, "letrec");
            } else if (head == s->sym_and) {
                st = handle_and_or(s, rest, env, /*is_or=*/0, &new_form, &new_env, &value, &result);
            } else if (head == s->sym_or) {
                st = handle_and_or(s, rest, env, /*is_or=*/1, &new_form, &new_env, &value, &result);
            } else if (head == s->sym_cond) {
                st = handle_cond(s, rest, env, &new_form, &new_env, &value, &result);
            }

            if (st != MICROLISP_OK) {
                ml_gc_restore(s, sp);
                return st;
            }
            if (result == SF_VALUE) {
                *out = value;
                ml_gc_restore(s, sp);
                return MICROLISP_OK;
            }
            if (result == SF_TCO) {
                s->gc_protect[form_slot] = new_form;
                s->gc_protect[env_slot] = new_env;
                continue;
            }
            /* SF_NOT_HANDLED: head is a regular bound symbol -- fall
             * through to procedure-call path. */
        }

        /* Procedure call. */
        mvalue proc;
        st = ml_eval(s, head, env, &proc);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        size_t sp_proc = ml_gc_savepoint(s);
        st = ml_gc_protect(s, proc);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }

        mvalue *argv = NULL;
        size_t argc = 0;
        st = eval_arg_list(s, rest, env, &argv, &argc);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }

        if (ml_is_prim(proc)) {
            mprim *p = ml_as_prim(proc);
            if (argc < p->arity_min || argc > p->arity_max) {
                ml_set_error(s, 0, 0, "%s: wrong number of arguments (got %zu)", p->name, argc);
                ml_raw_free(s, argv);
                ml_gc_unprotect(s, argc); /* arg values */
                ml_gc_restore(s, sp);
                return MICROLISP_ERR_ARITY;
            }
            mvalue value;
            st = p->fn(s, argc, argv, &value);
            ml_raw_free(s, argv);
            ml_gc_unprotect(s, argc);
            ml_gc_restore(s, sp_proc);
            if (st != MICROLISP_OK) {
                ml_gc_restore(s, sp);
                return st;
            }
            *out = value;
            ml_gc_restore(s, sp);
            return MICROLISP_OK;
        }

        if (ml_is_closure(proc)) {
            mclosure *cl = ml_as_closure(proc);
            mvalue new_env;
            st = apply_closure_bind(s, cl, argc, argv, &new_env);
            ml_raw_free(s, argv);
            ml_gc_unprotect(s, argc);
            if (st != MICROLISP_OK) {
                ml_gc_restore(s, sp);
                return st;
            }
            /* TCO into the closure body: form := (begin body...), env := new_env. */
            /* Run all but the last body form for effect; loop with the
             * last body form. Use begin_tco for uniformity. */
            sf_result result = SF_NOT_HANDLED;
            mvalue value = MV_UNDEF;
            mvalue new_form = MV_UNDEF;
            mvalue new_env2 = MV_UNDEF;
            st = begin_tco(s, cl->body, new_env, &new_form, &new_env2, &value, &result);
            ml_gc_restore(s, sp_proc);
            if (st != MICROLISP_OK) {
                ml_gc_restore(s, sp);
                return st;
            }
            if (result == SF_VALUE) {
                *out = value;
                ml_gc_restore(s, sp);
                return MICROLISP_OK;
            }
            /* SF_TCO */
            s->gc_protect[form_slot] = new_form;
            s->gc_protect[env_slot] = new_env2;
            continue;
        }

        ml_raw_free(s, argv);
        ml_gc_unprotect(s, argc);
        ml_set_error(s, 0, 0, "called value is not a procedure");
        ml_gc_restore(s, sp);
        return MICROLISP_ERR_TYPE;
    }
}
