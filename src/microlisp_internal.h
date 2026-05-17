/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * Internal definitions shared across the library's .c files. Not installed,
 * not part of the public ABI. Identifiers in here use the short `ml_` prefix
 * because they are file-local to the library and never reach the public
 * header; the public surface keeps the `microlisp_` prefix unchanged.
 */
#ifndef MICROLISP_INTERNAL_H_INCLUDED
#define MICROLISP_INTERNAL_H_INCLUDED

#include "microlisp/microlisp.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Value representation: tagged pointer in a uintptr_t.
 *
 * Low 3 bits of every value encode the type. Heap-object pointers are
 * 8-byte aligned (the allocator guarantees this with an aligned_alloc
 * fallback), so they naturally carry tag 0. Fixnums, symbol indices,
 * and immediate constants pack their payload in the high 61 bits.
 *
 * Layout:
 *
 *     ........ ........ ........ ........  tag (3 bits)   meaning
 *     -- 61-bit payload ----------------- 000             heap object *
 *     -- 60-bit signed fixnum ----------- 001             fixnum
 *     -- 60-bit immediate-id ------------ 010             immediate const
 *     -- 60-bit symbol index ------------ 011             interned symbol
 *
 * Tags 100-111 are reserved.
 *
 * A 64-bit target is required: 32-bit uintptr_t would compress the
 * fixnum range to 28 bits, which is too small for useful arithmetic.
 * -------------------------------------------------------------------------- */

_Static_assert(sizeof(uintptr_t) >= 8, "microlisp requires a 64-bit target");

typedef uintptr_t mvalue;

#define M_TAG_MASK ((mvalue)0x7U)
#define M_TAG_OBJ ((mvalue)0x0U)
#define M_TAG_FIX ((mvalue)0x1U)
#define M_TAG_IMM ((mvalue)0x2U)
#define M_TAG_SYM ((mvalue)0x3U)

/* v14 wants ((v)&M_TAG_MASK), v18+ wants ((v) & M_TAG_MASK); we go with v18+. */
/* clang-format off */
#define M_TAG_OF(v) ((v) & M_TAG_MASK)
/* clang-format on */
#define M_PAYLOAD(v) ((v) >> 3)

/* -- Immediates: identified by their id in the high bits. ------------------ */
#define M_IMM_ID_NIL 0U
#define M_IMM_ID_TRUE 1U
#define M_IMM_ID_FALSE 2U
#define M_IMM_ID_EOF 3U
#define M_IMM_ID_UNDEF 4U /* "no result" / unbound sentinel inside eval */

#define M_MAKE_IMM(id) ((mvalue)(((mvalue)(id) << 3) | M_TAG_IMM))
#define MV_NIL M_MAKE_IMM(M_IMM_ID_NIL)
#define MV_TRUE M_MAKE_IMM(M_IMM_ID_TRUE)
#define MV_FALSE M_MAKE_IMM(M_IMM_ID_FALSE)
#define MV_EOF M_MAKE_IMM(M_IMM_ID_EOF)
#define MV_UNDEF M_MAKE_IMM(M_IMM_ID_UNDEF)

/* -- Fixnum range. 60 bits signed: [-(2^59), 2^59 - 1]. -------------------- */
#define M_FIX_MIN ((int64_t) - ((int64_t)1 << 59))
#define M_FIX_MAX ((int64_t)(((int64_t)1 << 59) - 1))

/* Encode a signed 60-bit integer (caller must check range). */
static inline mvalue ml_make_fix(int64_t x) {
    /* Arithmetic shift in C is implementation-defined for negatives, so
     * pack/unpack via a 2's-complement reinterpretation that compiles
     * to a single shift on every mainstream target. */
    uint64_t u = (uint64_t)x;
    return (mvalue)((u << 3) | M_TAG_FIX);
}

/* Decode a fixnum. Undefined if v isn't tagged as a fixnum. */
static inline int64_t ml_fix_int(mvalue v) {
    int64_t s = (int64_t)v;
    return s >> 3; /* arithmetic shift preserves sign on all real targets */
}

/* -- Predicates ------------------------------------------------------------ */
static inline int ml_is_obj(mvalue v) {
    return M_TAG_OF(v) == M_TAG_OBJ && v != 0;
}
static inline int ml_is_fix(mvalue v) {
    return M_TAG_OF(v) == M_TAG_FIX;
}
static inline int ml_is_imm(mvalue v) {
    return M_TAG_OF(v) == M_TAG_IMM;
}
static inline int ml_is_sym(mvalue v) {
    return M_TAG_OF(v) == M_TAG_SYM;
}
static inline int ml_is_nil(mvalue v) {
    return v == MV_NIL;
}
static inline int ml_is_bool(mvalue v) {
    return v == MV_TRUE || v == MV_FALSE;
}
static inline int ml_truthy(mvalue v) {
    return v != MV_FALSE;
} /* Scheme: only #f is false */

static inline mvalue ml_make_bool(int b) {
    return b ? MV_TRUE : MV_FALSE;
}
static inline mvalue ml_make_sym(uint32_t idx) {
    return ((mvalue)idx << 3) | M_TAG_SYM;
}
static inline uint32_t ml_sym_index(mvalue v) {
    return (uint32_t)(v >> 3);
}

/* --------------------------------------------------------------------------
 * Heap objects.
 *
 * Every heap object starts with `struct mobj` so the GC can chain and
 * mark them uniformly. Concrete object types embed mobj as their
 * first member; downcasts go through ml_as_<type>(v).
 * -------------------------------------------------------------------------- */

typedef enum mobj_type {
    OBJ_PAIR = 1,
    OBJ_STRING = 2,
    OBJ_CLOSURE = 3,
    OBJ_PRIMITIVE = 4,
    OBJ_ENV = 5
} mobj_type;

typedef struct mobj {
    mobj_type type;
    uint8_t marked;    /**< Set during GC mark phase. */
    struct mobj *next; /**< Linked-list of all live heap objects. */
} mobj;

typedef struct mpair {
    mobj h;
    mvalue car;
    mvalue cdr;
} mpair;

typedef struct mstring {
    mobj h;
    size_t len;
    /* Trailing flex array: `bytes` is allocated as part of this struct. */
    uint8_t bytes[];
} mstring;

typedef struct mclosure {
    mobj h;
    mvalue env;         /**< Environment captured at lambda creation. */
    mvalue params;      /**< Proper or improper list of parameter symbols. */
    mvalue body;        /**< Non-empty list of body forms. */
    int variadic;       /**< 1 if params ends in a dotted rest-arg. */
    uint32_t arity_min; /**< Required positional params. */
} mclosure;

struct microlisp_state; /* forward */

typedef microlisp_status (*ml_prim_fn)(struct microlisp_state *state, size_t argc,
                                       const mvalue *argv, mvalue *out);

typedef struct mprim {
    mobj h;
    const char *name; /**< Static string; not owned. */
    ml_prim_fn fn;
    uint32_t arity_min; /**< Minimum required args. */
    uint32_t arity_max; /**< UINT32_MAX means "variadic". */
} mprim;

typedef struct menv {
    mobj h;
    mvalue parent; /**< Enclosing env, or MV_NIL for the top-level. */
    size_t count;
    size_t capacity;
    mvalue *names;  /**< Allocated array of symbol values (M_TAG_SYM). */
    mvalue *values; /**< Parallel array of bound values. */
} menv;

/* -- Heap-object accessors. Undefined if the tag is wrong. ----------------- */
static inline mobj *ml_as_obj(mvalue v) {
    return (mobj *)(uintptr_t)v;
}
static inline mpair *ml_as_pair(mvalue v) {
    return (mpair *)(uintptr_t)v;
}
static inline mstring *ml_as_string(mvalue v) {
    return (mstring *)(uintptr_t)v;
}
static inline mclosure *ml_as_closure(mvalue v) {
    return (mclosure *)(uintptr_t)v;
}
static inline mprim *ml_as_prim(mvalue v) {
    return (mprim *)(uintptr_t)v;
}
static inline menv *ml_as_env(mvalue v) {
    return (menv *)(uintptr_t)v;
}

static inline int ml_is_obj_type(mvalue v, mobj_type t) {
    return ml_is_obj(v) && ml_as_obj(v)->type == t;
}
static inline int ml_is_pair(mvalue v) {
    return ml_is_obj_type(v, OBJ_PAIR);
}
static inline int ml_is_string(mvalue v) {
    return ml_is_obj_type(v, OBJ_STRING);
}
static inline int ml_is_closure(mvalue v) {
    return ml_is_obj_type(v, OBJ_CLOSURE);
}
static inline int ml_is_prim(mvalue v) {
    return ml_is_obj_type(v, OBJ_PRIMITIVE);
}
static inline int ml_is_env(mvalue v) {
    return ml_is_obj_type(v, OBJ_ENV);
}
static inline int ml_is_procedure(mvalue v) {
    return ml_is_closure(v) || ml_is_prim(v);
}

/* --------------------------------------------------------------------------
 * Symbol table.
 *
 * Symbols are interned strings identified by a small integer index.
 * Lookup by name is O(n) for v0.1 -- a hash table is a clear v0.2 win
 * but adds bucket-rebalance complexity that isn't worth shipping
 * before the first review round. The whole table is freed on state
 * destroy; entries are never removed during a state's lifetime, so
 * symbol indices are stable and can be compared by identity.
 * -------------------------------------------------------------------------- */

typedef struct ml_symtab_entry {
    char *name; /**< NUL-terminated, owned. */
    size_t len;
} ml_symtab_entry;

typedef struct ml_symtab {
    ml_symtab_entry *entries;
    size_t count;
    size_t capacity;
} ml_symtab;

/* --------------------------------------------------------------------------
 * Error state.
 * -------------------------------------------------------------------------- */

typedef struct ml_error {
    char message[256];
    size_t line;   /**< 1-based; 0 = "no position." */
    size_t column; /**< 1-based; 0 = "no position." */
} ml_error;

/* --------------------------------------------------------------------------
 * The state.
 * -------------------------------------------------------------------------- */

struct microlisp_state {
    microlisp_allocator allocator; /**< Resolved (never partial). */
    int allocator_provided;        /**< 1 if user provided one; 0 means malloc. */

    /* GC ----------------------------------------------------------------- */
    mobj *gc_head;          /**< Head of "every heap object" list. */
    size_t gc_live_objects; /**< Live object count. */
    size_t gc_threshold;    /**< Trigger collection when live > threshold. */

    /* Protect stack: values that must survive the next collection but
     * aren't yet wired into a tracked structure. Reader/eval push and
     * pop entries here whenever they hold a heap-object handle in a
     * local. */
    mvalue *gc_protect;
    size_t gc_protect_count;
    size_t gc_protect_cap;

    /* Stress switch: when nonzero, every allocation triggers a full GC
     * before returning. Used by the gc-stress test to maximize the
     * chance of catching missing roots. */
    int gc_stress;

    /* Mark worklist: explicit-stack BFS used by ml_gc_collect to walk
     * the live graph iteratively. A purely recursive mark would
     * overflow the C stack on a heap-built linked list with several
     * thousand cons cells (which fuzz_read produces routinely from
     * tail-recursive accumulator-style programs). The worklist lives
     * on the state so allocation overhead amortizes across collections. */
    mobj **gc_marklist;
    size_t gc_marklist_count;
    size_t gc_marklist_cap;

    /* Symbol table ------------------------------------------------------- */
    ml_symtab symtab;

    /* Pre-interned symbols for special forms and frequently used names.
     * Stored once so reader / eval can compare by integer identity
     * (mvalue equality) without rehashing. */
    mvalue sym_quote;
    mvalue sym_if;
    mvalue sym_define;
    mvalue sym_lambda;
    mvalue sym_set_bang;
    mvalue sym_let;
    mvalue sym_let_star;
    mvalue sym_letrec;
    mvalue sym_begin;
    mvalue sym_and;
    mvalue sym_or;
    mvalue sym_cond;
    mvalue sym_else;

    /* Top-level environment ---------------------------------------------- */
    mvalue toplevel_env;

    /* Reader options ----------------------------------------------------- */
    size_t max_read_depth;

    /* Eval depth limiter. eval_depth is the live counter; max_eval_depth
     * is the configured ceiling. ml_eval increments on entry and
     * decrements on exit. Tail-call optimization means a long tail-
     * recursive program touches this exactly once per non-tail call,
     * not per loop iteration. */
    size_t max_eval_depth;
    size_t eval_depth;

    /* Printer depth limiter: same shape as eval-depth. print_value
     * increments on every recursive pair-car descent and bails with
     * MICROLISP_ERR_PRINT_DEPTH if the limit is exceeded. v0.2 will
     * replace the recursive walker with an explicit-stack BFS in
     * the spirit of the GC mark fix in v0.1.1. */
    size_t max_print_depth;
    size_t print_depth;

    /* Structural-equality depth limiter: same shape as the printer's.
     * ml_value_equal / ml_value_eqv walk pair-car chains recursively;
     * the cap keeps the host C stack bounded when comparing two
     * deeply-nested structures (a fuzz / DoS surface). */
    size_t max_equal_depth;
    size_t equal_depth;

    /* Output sink used by the `display`, `write`, and `newline`
     * primitives. Defaults to stdout; microlisp_repl temporarily
     * swaps in the caller-supplied out_file so REPL output honors
     * the API contract that everything goes through that FILE *. */
    FILE *output;

    /* Last error --------------------------------------------------------- */
    ml_error last_error;
};

typedef struct microlisp_state ml_state;

/* --------------------------------------------------------------------------
 * Allocator helpers.
 * -------------------------------------------------------------------------- */

void *ml_raw_alloc(ml_state *s, size_t size);
void ml_raw_free(ml_state *s, void *p);

/* --------------------------------------------------------------------------
 * GC.
 * -------------------------------------------------------------------------- */

microlisp_status ml_gc_alloc_pair(ml_state *s, mvalue car, mvalue cdr, mvalue *out);
microlisp_status ml_gc_alloc_string(ml_state *s, const uint8_t *bytes, size_t len, mvalue *out);
microlisp_status ml_gc_alloc_closure(ml_state *s, mvalue env, mvalue params, mvalue body,
                                     mvalue *out);
microlisp_status ml_gc_alloc_primitive(ml_state *s, const char *name, ml_prim_fn fn,
                                       uint32_t arity_min, uint32_t arity_max, mvalue *out);
microlisp_status ml_gc_alloc_env(ml_state *s, mvalue parent, mvalue *out);

/* Push @p v onto the protect stack. Returns OK or ERR_NOMEM if grow failed. */
microlisp_status ml_gc_protect(ml_state *s, mvalue v);

/* Pop the most recent N entries from the protect stack. */
void ml_gc_unprotect(ml_state *s, size_t n);

/* Inspect / restore the protect stack height. Use for early-return paths. */
static inline size_t ml_gc_savepoint(const ml_state *s) {
    return s->gc_protect_count;
}
void ml_gc_restore(ml_state *s, size_t saved);

/* Force a full collection now. */
void ml_gc_collect(ml_state *s);

/* Free every heap object (called by microlisp_state_destroy). */
void ml_gc_free_all(ml_state *s);

/* --------------------------------------------------------------------------
 * Symbol table.
 * -------------------------------------------------------------------------- */

microlisp_status ml_sym_intern(ml_state *s, const char *name, size_t len, mvalue *out);
const char *ml_sym_name(const ml_state *s, mvalue sym, size_t *out_len);

/* --------------------------------------------------------------------------
 * Environment.
 * -------------------------------------------------------------------------- */

/* Define a fresh binding in @p env. If @p name already exists, replace it. */
microlisp_status ml_env_define(ml_state *s, mvalue env, mvalue name, mvalue value);

/* Set an existing binding in @p env or any enclosing env. Returns
 * ERR_UNBOUND if the name doesn't exist. */
microlisp_status ml_env_set(ml_state *s, mvalue env, mvalue name, mvalue value);

/* Look up @p name in @p env or any enclosing env. Returns ERR_UNBOUND
 * if not found. */
microlisp_status ml_env_lookup(const ml_state *s, mvalue env, mvalue name, mvalue *out);

/* --------------------------------------------------------------------------
 * Reader / printer / evaluator (declarations here; impl across files).
 * -------------------------------------------------------------------------- */

typedef struct ml_reader {
    const uint8_t *input;
    size_t len;
    size_t pos;
    size_t line;
    size_t column;
    size_t depth; /**< Current paren nesting. */
} ml_reader;

/* Read the next top-level form. Sets @p *out to MV_EOF when input is
 * exhausted (with status OK). */
microlisp_status ml_read(ml_state *s, ml_reader *r, mvalue *out);

/* Printer. write_style=1 prints in read-back form (strings quoted,
 * escapes escaped); write_style=0 is display form. */
typedef microlisp_status (*ml_out_fn)(const void *bytes, size_t len, void *user);
microlisp_status ml_print(ml_state *s, mvalue v, int write_style, ml_out_fn out, void *user);

/* Convenience: print to a freshly allocated NUL-terminated buffer. */
microlisp_status ml_print_to_alloc(ml_state *s, mvalue v, int write_style, char **out_bytes,
                                   size_t *out_len);

/* Evaluator entry point. */
microlisp_status ml_eval(ml_state *s, mvalue form, mvalue env, mvalue *out);

/* Install the built-in primitives into @p env (typically the top-level). */
microlisp_status ml_primitives_install(ml_state *s, mvalue env);

/* --------------------------------------------------------------------------
 * Value equality (value.c).
 * -------------------------------------------------------------------------- */

int ml_value_eq(mvalue a, mvalue b);
int ml_value_eqv(mvalue a, mvalue b);
/* Structural equality with bounded recursion depth. Walks pairs
 * recursively; bails with MICROLISP_ERR_EQUAL_DEPTH if depth exceeds
 * ml_state::max_equal_depth. Returns the equality result through
 * @p out_equal on success. */
microlisp_status ml_value_equal(ml_state *s, mvalue a, mvalue b, int *out_equal);

/* --------------------------------------------------------------------------
 * Error helpers.
 * -------------------------------------------------------------------------- */

/* Set @p s->last_error. The position is reported as 0,0 when not known. */
void ml_set_error(ml_state *s, size_t line, size_t column, const char *fmt, ...)
    MICROLISP_PRINTF(4, 5);

/* Clear the last error (called at the top of microlisp_eval). */
void ml_clear_error(ml_state *s);

#endif /* MICROLISP_INTERNAL_H_INCLUDED */
