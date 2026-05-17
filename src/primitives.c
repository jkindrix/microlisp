/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * Built-in procedures bound into the top-level environment.
 *
 * Every primitive has signature
 *
 *     microlisp_status p(ml_state *, size_t argc, const mvalue *argv,
 *                        mvalue *out);
 *
 * The evaluator validates argc against the registered arity range
 * before calling, so each primitive can assume `argc` falls within
 * `[arity_min, arity_max]`. Type checking is the primitive's own
 * responsibility -- the evaluator doesn't know what types each one
 * expects.
 *
 * Integer arithmetic uses checked builtins so overflow surfaces as
 * MICROLISP_ERR_OVERFLOW rather than wrapping silently.
 */

#include "microlisp_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Type-check helpers.
 * -------------------------------------------------------------------------- */

#define EXPECT_FIX(state, v, what)                                                                 \
    do {                                                                                           \
        if (!ml_is_fix(v)) {                                                                       \
            ml_set_error((state), 0, 0, "%s: expected number", (what));                            \
            return MICROLISP_ERR_TYPE;                                                             \
        }                                                                                          \
    } while (0)

static int fits_fixnum(int64_t v) {
    return v >= M_FIX_MIN && v <= M_FIX_MAX;
}

/* --------------------------------------------------------------------------
 * Arithmetic.
 * -------------------------------------------------------------------------- */

static microlisp_status prim_add(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    int64_t acc = 0;
    for (size_t i = 0; i < argc; i++) {
        EXPECT_FIX(s, argv[i], "+");
        int64_t v = ml_fix_int(argv[i]);
        int64_t sum;
        if (__builtin_add_overflow(acc, v, &sum) || !fits_fixnum(sum)) {
            ml_set_error(s, 0, 0, "+: integer overflow");
            return MICROLISP_ERR_OVERFLOW;
        }
        acc = sum;
    }
    *out = ml_make_fix(acc);
    return MICROLISP_OK;
}

static microlisp_status prim_sub(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    if (argc == 1) {
        EXPECT_FIX(s, argv[0], "-");
        int64_t v = -ml_fix_int(argv[0]);
        if (!fits_fixnum(v)) {
            ml_set_error(s, 0, 0, "-: integer overflow");
            return MICROLISP_ERR_OVERFLOW;
        }
        *out = ml_make_fix(v);
        return MICROLISP_OK;
    }
    EXPECT_FIX(s, argv[0], "-");
    int64_t acc = ml_fix_int(argv[0]);
    for (size_t i = 1; i < argc; i++) {
        EXPECT_FIX(s, argv[i], "-");
        int64_t v = ml_fix_int(argv[i]);
        int64_t diff;
        if (__builtin_sub_overflow(acc, v, &diff) || !fits_fixnum(diff)) {
            ml_set_error(s, 0, 0, "-: integer overflow");
            return MICROLISP_ERR_OVERFLOW;
        }
        acc = diff;
    }
    *out = ml_make_fix(acc);
    return MICROLISP_OK;
}

static microlisp_status prim_mul(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    int64_t acc = 1;
    for (size_t i = 0; i < argc; i++) {
        EXPECT_FIX(s, argv[i], "*");
        int64_t v = ml_fix_int(argv[i]);
        int64_t prod;
        if (__builtin_mul_overflow(acc, v, &prod) || !fits_fixnum(prod)) {
            ml_set_error(s, 0, 0, "*: integer overflow");
            return MICROLISP_ERR_OVERFLOW;
        }
        acc = prod;
    }
    *out = ml_make_fix(acc);
    return MICROLISP_OK;
}

/* Scheme `/` on integers in v0.1 = quotient. (R5RS would return an
 * exact rational; we don't have rationals.) */
static microlisp_status prim_div(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    if (argc == 1) {
        EXPECT_FIX(s, argv[0], "/");
        int64_t v = ml_fix_int(argv[0]);
        if (v == 0) {
            ml_set_error(s, 0, 0, "/: division by zero");
            return MICROLISP_ERR_DIV_ZERO;
        }
        /* 1/v in integer arithmetic is 0 except for v in {1, -1}. */
        int64_t r = v == 1 ? 1 : (v == -1 ? -1 : 0);
        *out = ml_make_fix(r);
        return MICROLISP_OK;
    }
    EXPECT_FIX(s, argv[0], "/");
    int64_t acc = ml_fix_int(argv[0]);
    for (size_t i = 1; i < argc; i++) {
        EXPECT_FIX(s, argv[i], "/");
        int64_t v = ml_fix_int(argv[i]);
        if (v == 0) {
            ml_set_error(s, 0, 0, "/: division by zero");
            return MICROLISP_ERR_DIV_ZERO;
        }
        if (acc == INT64_MIN && v == -1) {
            ml_set_error(s, 0, 0, "/: integer overflow");
            return MICROLISP_ERR_OVERFLOW;
        }
        acc /= v;
    }
    *out = ml_make_fix(acc);
    return MICROLISP_OK;
}

static microlisp_status prim_quotient(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)argc;
    EXPECT_FIX(s, argv[0], "quotient");
    EXPECT_FIX(s, argv[1], "quotient");
    int64_t a = ml_fix_int(argv[0]);
    int64_t b = ml_fix_int(argv[1]);
    if (b == 0) {
        ml_set_error(s, 0, 0, "quotient: division by zero");
        return MICROLISP_ERR_DIV_ZERO;
    }
    if (a == INT64_MIN && b == -1) {
        ml_set_error(s, 0, 0, "quotient: integer overflow");
        return MICROLISP_ERR_OVERFLOW;
    }
    *out = ml_make_fix(a / b);
    return MICROLISP_OK;
}

static microlisp_status prim_remainder(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)argc;
    EXPECT_FIX(s, argv[0], "remainder");
    EXPECT_FIX(s, argv[1], "remainder");
    int64_t a = ml_fix_int(argv[0]);
    int64_t b = ml_fix_int(argv[1]);
    if (b == 0) {
        ml_set_error(s, 0, 0, "remainder: division by zero");
        return MICROLISP_ERR_DIV_ZERO;
    }
    if (a == INT64_MIN && b == -1) {
        /* a % -1 is 0 mathematically; C produces 0 but a/-1 trapped
         * just above. Special-case so we return 0 cleanly. */
        *out = ml_make_fix(0);
        return MICROLISP_OK;
    }
    *out = ml_make_fix(a % b);
    return MICROLISP_OK;
}

static microlisp_status prim_modulo(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)argc;
    EXPECT_FIX(s, argv[0], "modulo");
    EXPECT_FIX(s, argv[1], "modulo");
    int64_t a = ml_fix_int(argv[0]);
    int64_t b = ml_fix_int(argv[1]);
    if (b == 0) {
        ml_set_error(s, 0, 0, "modulo: division by zero");
        return MICROLISP_ERR_DIV_ZERO;
    }
    /* R5RS modulo: result has sign of divisor. C's % has sign of
     * dividend, so add divisor if the signs disagree. */
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        r += b;
    }
    *out = ml_make_fix(r);
    return MICROLISP_OK;
}

/* --------------------------------------------------------------------------
 * Comparison.
 * -------------------------------------------------------------------------- */

typedef enum { CMP_EQ, CMP_LT, CMP_GT, CMP_LE, CMP_GE } cmp_op;

static microlisp_status cmp_chain(ml_state *s, size_t argc, const mvalue *argv, cmp_op op,
                                  const char *name, mvalue *out) {
    EXPECT_FIX(s, argv[0], name);
    int64_t prev = ml_fix_int(argv[0]);
    for (size_t i = 1; i < argc; i++) {
        EXPECT_FIX(s, argv[i], name);
        int64_t cur = ml_fix_int(argv[i]);
        int ok = 0;
        switch (op) {
        case CMP_EQ:
            ok = prev == cur;
            break;
        case CMP_LT:
            ok = prev < cur;
            break;
        case CMP_GT:
            ok = prev > cur;
            break;
        case CMP_LE:
            ok = prev <= cur;
            break;
        case CMP_GE:
            ok = prev >= cur;
            break;
        default:
            ok = 0;
            break;
        }
        if (!ok) {
            *out = MV_FALSE;
            return MICROLISP_OK;
        }
        prev = cur;
    }
    *out = MV_TRUE;
    return MICROLISP_OK;
}

static microlisp_status prim_num_eq(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    return cmp_chain(s, argc, argv, CMP_EQ, "=", out);
}
static microlisp_status prim_num_lt(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    return cmp_chain(s, argc, argv, CMP_LT, "<", out);
}
static microlisp_status prim_num_gt(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    return cmp_chain(s, argc, argv, CMP_GT, ">", out);
}
static microlisp_status prim_num_le(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    return cmp_chain(s, argc, argv, CMP_LE, "<=", out);
}
static microlisp_status prim_num_ge(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    return cmp_chain(s, argc, argv, CMP_GE, ">=", out);
}

/* --------------------------------------------------------------------------
 * Pairs & lists.
 * -------------------------------------------------------------------------- */

static microlisp_status prim_cons(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)argc;
    return ml_gc_alloc_pair(s, argv[0], argv[1], out);
}

static microlisp_status prim_car(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)argc;
    if (!ml_is_pair(argv[0])) {
        ml_set_error(s, 0, 0, "car: argument is not a pair");
        return MICROLISP_ERR_TYPE;
    }
    *out = ml_as_pair(argv[0])->car;
    return MICROLISP_OK;
}

static microlisp_status prim_cdr(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)argc;
    if (!ml_is_pair(argv[0])) {
        ml_set_error(s, 0, 0, "cdr: argument is not a pair");
        return MICROLISP_ERR_TYPE;
    }
    *out = ml_as_pair(argv[0])->cdr;
    return MICROLISP_OK;
}

static microlisp_status prim_list(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    mvalue head = MV_NIL;
    size_t sp = ml_gc_savepoint(s);
    microlisp_status st = ml_gc_protect(s, head);
    if (st != MICROLISP_OK) {
        return st;
    }
    for (size_t i = argc; i > 0; i--) {
        mvalue cell;
        st = ml_gc_alloc_pair(s, argv[i - 1], head, &cell);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        head = cell;
        s->gc_protect[sp] = head;
    }
    *out = head;
    ml_gc_restore(s, sp);
    return MICROLISP_OK;
}

static microlisp_status prim_length(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)argc;
    mvalue v = argv[0];
    size_t n = 0;
    while (ml_is_pair(v)) {
        n++;
        v = ml_as_pair(v)->cdr;
    }
    if (v != MV_NIL) {
        ml_set_error(s, 0, 0, "length: argument is not a proper list");
        return MICROLISP_ERR_TYPE;
    }
    if ((int64_t)n > M_FIX_MAX) {
        ml_set_error(s, 0, 0, "length: result exceeds fixnum range");
        return MICROLISP_ERR_OVERFLOW;
    }
    *out = ml_make_fix((int64_t)n);
    return MICROLISP_OK;
}

/* --------------------------------------------------------------------------
 * Predicates.
 * -------------------------------------------------------------------------- */

#define DEFINE_PREDICATE(name, expr)                                                               \
    static microlisp_status prim_##name(ml_state *s, size_t argc, const mvalue *argv,              \
                                        mvalue *out) {                                             \
        (void)s;                                                                                   \
        (void)argc;                                                                                \
        *out = ml_make_bool(expr);                                                                 \
        return MICROLISP_OK;                                                                       \
    }

DEFINE_PREDICATE(null_p, argv[0] == MV_NIL)
DEFINE_PREDICATE(pair_p, ml_is_pair(argv[0]))
DEFINE_PREDICATE(number_p, ml_is_fix(argv[0]))
DEFINE_PREDICATE(symbol_p, ml_is_sym(argv[0]))
DEFINE_PREDICATE(string_p, ml_is_string(argv[0]))
DEFINE_PREDICATE(procedure_p, ml_is_procedure(argv[0]))
DEFINE_PREDICATE(boolean_p, ml_is_bool(argv[0]))

static microlisp_status prim_eq_p(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)s;
    (void)argc;
    *out = ml_make_bool(ml_value_eq(argv[0], argv[1]));
    return MICROLISP_OK;
}

static microlisp_status prim_eqv_p(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)s;
    (void)argc;
    *out = ml_make_bool(ml_value_eqv(argv[0], argv[1]));
    return MICROLISP_OK;
}

static microlisp_status prim_equal_p(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)s;
    (void)argc;
    *out = ml_make_bool(ml_value_equal(argv[0], argv[1]));
    return MICROLISP_OK;
}

static microlisp_status prim_not(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)s;
    (void)argc;
    *out = ml_make_bool(!ml_truthy(argv[0]));
    return MICROLISP_OK;
}

/* --------------------------------------------------------------------------
 * I/O.
 * -------------------------------------------------------------------------- */

static microlisp_status stdout_sink(const void *bytes, size_t len, void *user) {
    (void)user;
    if (fwrite(bytes, 1, len, stdout) != len) {
        return MICROLISP_ERR_IO;
    }
    return MICROLISP_OK;
}

/* Flush so a broken pipe surfaces as ERR_IO at the point of writing
 * rather than silently at process exit -- this is what makes
 * `microlisp -e '(display ...)' | head -n 0` return our documented
 * exit code 2 instead of 0. */
static microlisp_status stdout_flush(void) {
    return fflush(stdout) == 0 ? MICROLISP_OK : MICROLISP_ERR_IO;
}

static microlisp_status prim_display(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)argc;
    microlisp_status st = ml_print(s, argv[0], /*write_style=*/0, stdout_sink, NULL);
    if (st != MICROLISP_OK) {
        return st;
    }
    st = stdout_flush();
    if (st != MICROLISP_OK) {
        return st;
    }
    *out = MV_UNDEF;
    return MICROLISP_OK;
}

static microlisp_status prim_write(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)argc;
    microlisp_status st = ml_print(s, argv[0], /*write_style=*/1, stdout_sink, NULL);
    if (st != MICROLISP_OK) {
        return st;
    }
    st = stdout_flush();
    if (st != MICROLISP_OK) {
        return st;
    }
    *out = MV_UNDEF;
    return MICROLISP_OK;
}

static microlisp_status prim_newline(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)s;
    (void)argc;
    (void)argv;
    if (fputc('\n', stdout) == EOF) {
        return MICROLISP_ERR_IO;
    }
    microlisp_status st = stdout_flush();
    if (st != MICROLISP_OK) {
        return st;
    }
    *out = MV_UNDEF;
    return MICROLISP_OK;
}

/* NOLINTNEXTLINE(readability-non-const-parameter) */
static microlisp_status prim_error(ml_state *s, size_t argc, const mvalue *argv, mvalue *out) {
    (void)out;
    if (argc == 0) {
        ml_set_error(s, 0, 0, "error");
        return MICROLISP_ERR_USER;
    }
    if (ml_is_string(argv[0])) {
        mstring *m = ml_as_string(argv[0]);
        size_t copy =
            m->len < sizeof s->last_error.message - 1 ? m->len : sizeof s->last_error.message - 1;
        memcpy(s->last_error.message, m->bytes, copy);
        s->last_error.message[copy] = '\0';
        s->last_error.line = 0;
        s->last_error.column = 0;
    } else if (ml_is_sym(argv[0])) {
        size_t len;
        const char *name = ml_sym_name(s, argv[0], &len);
        ml_set_error(s, 0, 0, "%.*s", (int)len, name);
    } else {
        ml_set_error(s, 0, 0, "error");
    }
    return MICROLISP_ERR_USER;
}

/* --------------------------------------------------------------------------
 * Registry.
 * -------------------------------------------------------------------------- */

typedef struct prim_decl {
    const char *name;
    ml_prim_fn fn;
    uint32_t arity_min;
    uint32_t arity_max; /**< UINT32_MAX = variadic. */
} prim_decl;

static const prim_decl k_primitives[] = {
    /* Arithmetic */
    {"+", prim_add, 0, UINT32_MAX},
    {"-", prim_sub, 1, UINT32_MAX},
    {"*", prim_mul, 0, UINT32_MAX},
    {"/", prim_div, 1, UINT32_MAX},
    {"quotient", prim_quotient, 2, 2},
    {"remainder", prim_remainder, 2, 2},
    {"modulo", prim_modulo, 2, 2},

    /* Comparison */
    {"=", prim_num_eq, 2, UINT32_MAX},
    {"<", prim_num_lt, 2, UINT32_MAX},
    {">", prim_num_gt, 2, UINT32_MAX},
    {"<=", prim_num_le, 2, UINT32_MAX},
    {">=", prim_num_ge, 2, UINT32_MAX},

    /* List operations */
    {"cons", prim_cons, 2, 2},
    {"car", prim_car, 1, 1},
    {"cdr", prim_cdr, 1, 1},
    {"list", prim_list, 0, UINT32_MAX},
    {"length", prim_length, 1, 1},

    /* Predicates */
    {"null?", prim_null_p, 1, 1},
    {"pair?", prim_pair_p, 1, 1},
    {"number?", prim_number_p, 1, 1},
    {"symbol?", prim_symbol_p, 1, 1},
    {"string?", prim_string_p, 1, 1},
    {"procedure?", prim_procedure_p, 1, 1},
    {"boolean?", prim_boolean_p, 1, 1},
    {"eq?", prim_eq_p, 2, 2},
    {"eqv?", prim_eqv_p, 2, 2},
    {"equal?", prim_equal_p, 2, 2},
    {"not", prim_not, 1, 1},

    /* I/O */
    {"display", prim_display, 1, 1},
    {"write", prim_write, 1, 1},
    {"newline", prim_newline, 0, 0},
    {"error", prim_error, 0, UINT32_MAX},
};

microlisp_status ml_primitives_install(ml_state *s, mvalue env) {
    for (size_t i = 0; i < sizeof k_primitives / sizeof k_primitives[0]; i++) {
        const prim_decl *d = &k_primitives[i];
        mvalue prim;
        microlisp_status st =
            ml_gc_alloc_primitive(s, d->name, d->fn, d->arity_min, d->arity_max, &prim);
        if (st != MICROLISP_OK) {
            return st;
        }
        size_t sp = ml_gc_savepoint(s);
        st = ml_gc_protect(s, prim);
        if (st != MICROLISP_OK) {
            return st;
        }
        mvalue sym;
        st = ml_sym_intern(s, d->name, strlen(d->name), &sym);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        st = ml_env_define(s, env, sym, prim);
        ml_gc_restore(s, sp);
        if (st != MICROLISP_OK) {
            return st;
        }
    }
    return MICROLISP_OK;
}
