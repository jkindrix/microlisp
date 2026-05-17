/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * State lifecycle, error reporting, and the high-level microlisp_eval
 * and microlisp_repl entry points.
 */

#include "microlisp_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Allocator validation + resolution.
 * -------------------------------------------------------------------------- */

static void *default_alloc(size_t size, void *user) {
    (void)user;
    return malloc(size);
}

static void default_free(void *ptr, void *user) {
    (void)user;
    free(ptr);
}

static microlisp_status validate_allocator(const microlisp_allocator *a) {
    if (a == NULL) {
        return MICROLISP_OK;
    }
    if (a->alloc == NULL || a->free == NULL) {
        /* Partial table: refuse rather than silently mix the user's
         * alloc with stdlib free (or vice versa). */
        return MICROLISP_ERR_INVALID_ARG;
    }
    return MICROLISP_OK;
}

/* --------------------------------------------------------------------------
 * Error helpers.
 * -------------------------------------------------------------------------- */

void ml_clear_error(ml_state *s) {
    s->last_error.message[0] = '\0';
    s->last_error.line = 0;
    s->last_error.column = 0;
}

void ml_set_error(ml_state *s, size_t line, size_t column, const char *fmt, ...) {
    s->last_error.line = line;
    s->last_error.column = column;
    va_list ap;
    va_start(ap, fmt);
    /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) - va_start above. */
    int written = vsnprintf(s->last_error.message, sizeof s->last_error.message, fmt, ap);
    va_end(ap);
    if (written < 0) {
        s->last_error.message[0] = '\0';
    }
}

const char *microlisp_state_error(const microlisp_state *state) {
    if (state == NULL) {
        return "";
    }
    return state->last_error.message;
}

void microlisp_state_error_position(const microlisp_state *state, size_t *out_line,
                                    size_t *out_column) {
    size_t line = 0;
    size_t column = 0;
    if (state != NULL) {
        line = state->last_error.line;
        column = state->last_error.column;
    }
    if (out_line != NULL) {
        *out_line = line;
    }
    if (out_column != NULL) {
        *out_column = column;
    }
}

/* --------------------------------------------------------------------------
 * Create / destroy.
 * -------------------------------------------------------------------------- */

static microlisp_status intern_special_forms(ml_state *s) {
    struct {
        mvalue *slot;
        const char *name;
    } table[] = {
        {&s->sym_quote, "quote"},   {&s->sym_if, "if"},         {&s->sym_define, "define"},
        {&s->sym_lambda, "lambda"}, {&s->sym_set_bang, "set!"}, {&s->sym_let, "let"},
        {&s->sym_let_star, "let*"}, {&s->sym_letrec, "letrec"}, {&s->sym_begin, "begin"},
        {&s->sym_and, "and"},       {&s->sym_or, "or"},         {&s->sym_cond, "cond"},
        {&s->sym_else, "else"},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        microlisp_status st = ml_sym_intern(s, table[i].name, strlen(table[i].name), table[i].slot);
        if (st != MICROLISP_OK) {
            return st;
        }
    }
    return MICROLISP_OK;
}

microlisp_status microlisp_state_create(const microlisp_options *opts,
                                        microlisp_state **out_state) {
    if (out_state == NULL) {
        return MICROLISP_ERR_INVALID_ARG;
    }
    *out_state = NULL;

    const microlisp_allocator *user_alloc = (opts != NULL) ? opts->allocator : NULL;
    microlisp_status st = validate_allocator(user_alloc);
    if (st != MICROLISP_OK) {
        return st;
    }

    /* Allocate the state itself via whichever allocator the caller chose,
     * so subsequent frees use the matching free. */
    void *(*alloc_fn)(size_t, void *) = user_alloc != NULL ? user_alloc->alloc : default_alloc;
    void (*free_fn)(void *, void *) = user_alloc != NULL ? user_alloc->free : default_free;
    void *user_data = user_alloc != NULL ? user_alloc->user : NULL;

    microlisp_state *s = (microlisp_state *)alloc_fn(sizeof(microlisp_state), user_data);
    if (s == NULL) {
        return MICROLISP_ERR_NOMEM;
    }
    memset(s, 0, sizeof *s);
    s->allocator.alloc = alloc_fn;
    s->allocator.free = free_fn;
    s->allocator.user = user_data;
    s->allocator_provided = (user_alloc != NULL);

    s->max_read_depth = (opts != NULL && opts->max_read_depth != 0)
                            ? opts->max_read_depth
                            : MICROLISP_DEFAULT_MAX_READ_DEPTH;
    s->max_eval_depth = (opts != NULL && opts->max_eval_depth != 0)
                            ? opts->max_eval_depth
                            : MICROLISP_DEFAULT_MAX_EVAL_DEPTH;
    s->gc_threshold =
        (opts != NULL && opts->gc_initial_threshold != 0) ? opts->gc_initial_threshold : 4096;

    /* Intern the special-form symbols and build the top-level environment. */
    st = intern_special_forms(s);
    if (st != MICROLISP_OK) {
        goto fail;
    }
    st = ml_gc_alloc_env(s, MV_NIL, &s->toplevel_env);
    if (st != MICROLISP_OK) {
        goto fail;
    }
    st = ml_primitives_install(s, s->toplevel_env);
    if (st != MICROLISP_OK) {
        goto fail;
    }

    *out_state = s;
    return MICROLISP_OK;

fail:
    microlisp_state_destroy(s);
    return st;
}

void microlisp_state_destroy(microlisp_state *state) {
    if (state == NULL) {
        return;
    }
    /* Drop GC roots so nothing survives. */
    state->toplevel_env = MV_NIL;
    state->gc_protect_count = 0;
    ml_gc_free_all(state);

    /* Symbol table. */
    for (size_t i = 0; i < state->symtab.count; i++) {
        ml_raw_free(state, state->symtab.entries[i].name);
    }
    ml_raw_free(state, state->symtab.entries);

    ml_raw_free(state, state->gc_protect);

    /* Self. */
    void (*free_fn)(void *, void *) = state->allocator.free;
    void *user_data = state->allocator.user;
    free_fn(state, user_data);
}

/* --------------------------------------------------------------------------
 * Eval entry point.
 *
 * Reads top-level forms from `source` and evaluates them in the
 * top-level environment. The result of the last form is printed (in
 * write-style) into an owned buffer that the caller releases via
 * microlisp_buffer_free.
 *
 * Returns OK with `*out_bytes == NULL` and `*out_len == 0` when the
 * source contains no forms (empty or whitespace only).
 * -------------------------------------------------------------------------- */

microlisp_status microlisp_eval(microlisp_state *state, const char *source, size_t source_len,
                                char **out_bytes, size_t *out_len) {
    if (state == NULL) {
        return MICROLISP_ERR_INVALID_ARG;
    }
    if (out_bytes != NULL) {
        *out_bytes = NULL;
    }
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (source == NULL && source_len != 0) {
        ml_set_error(state, 0, 0, "source pointer is NULL but length is %zu", source_len);
        return MICROLISP_ERR_INVALID_ARG;
    }

    ml_clear_error(state);

    ml_reader r = {
        .input = (const uint8_t *)source,
        .len = source_len,
        .pos = 0,
        .line = 1,
        .column = 1,
        .depth = 0,
    };

    mvalue last_result = MV_UNDEF;
    int saw_form = 0;

    for (;;) {
        mvalue form;
        microlisp_status st = ml_read(state, &r, &form);
        if (st != MICROLISP_OK) {
            return st;
        }
        if (form == MV_EOF) {
            break;
        }
        size_t sp = ml_gc_savepoint(state);
        st = ml_gc_protect(state, form);
        if (st != MICROLISP_OK) {
            return st;
        }
        st = ml_eval(state, form, state->toplevel_env, &last_result);
        ml_gc_restore(state, sp);
        if (st != MICROLISP_OK) {
            return st;
        }
        saw_form = 1;
    }

    if (!saw_form) {
        return MICROLISP_OK;
    }

    if (out_bytes != NULL) {
        /* Suppress unspecified-value output (R5RS: the result of forms
         * like (display ...), (newline), (define ...), (set! ...) is
         * unspecified -- many Schemes print nothing). Callers that
         * want to see the sentinel can detect a NULL out_bytes with
         * status OK. */
        if (last_result == MV_UNDEF) {
            return MICROLISP_OK;
        }
        size_t sp = ml_gc_savepoint(state);
        microlisp_status st = ml_gc_protect(state, last_result);
        if (st != MICROLISP_OK) {
            return st;
        }
        char *buf = NULL;
        size_t len = 0;
        st = ml_print_to_alloc(state, last_result, /*write_style=*/1, &buf, &len);
        ml_gc_restore(state, sp);
        if (st != MICROLISP_OK) {
            return st;
        }
        *out_bytes = buf;
        if (out_len != NULL) {
            *out_len = len;
        }
    }
    return MICROLISP_OK;
}

void microlisp_buffer_free(microlisp_state *state, char *bytes) {
    if (state == NULL || bytes == NULL) {
        return;
    }
    ml_raw_free(state, bytes);
}

/* --------------------------------------------------------------------------
 * REPL.
 *
 * Reads one form per line; each form's result is printed back. Errors
 * are reported and the loop continues with the next prompt.
 * -------------------------------------------------------------------------- */

static int fputs_or_fail(FILE *out, const char *s) {
    return fputs(s, out) == EOF ? -1 : 0;
}

microlisp_status microlisp_repl(microlisp_state *state, FILE *in_file, FILE *out_file,
                                const char *prompt) {
    if (state == NULL) {
        return MICROLISP_ERR_INVALID_ARG;
    }
    if (in_file == NULL) {
        in_file = stdin;
    }
    if (out_file == NULL) {
        out_file = stdout;
    }

    /* Line-based REPL: read one line, eval as a (possibly empty)
     * top-level program. v0.1 doesn't support multi-line forms; a
     * partial-form continuation prompt is a v0.2 ergonomics win. */
    char buf[4096];
    for (;;) {
        if (prompt != NULL) {
            if (fputs_or_fail(out_file, prompt) != 0 || fflush(out_file) == EOF) {
                return MICROLISP_ERR_IO;
            }
        }
        if (fgets(buf, sizeof buf, in_file) == NULL) {
            if (prompt != NULL) {
                /* Tidy trailing newline so the shell prompt isn't smudged. */
                if (fputs_or_fail(out_file, "\n") != 0) {
                    return MICROLISP_ERR_IO;
                }
            }
            return MICROLISP_OK;
        }
        size_t n = strlen(buf);
        if (n == 0) {
            continue;
        }
        char *result = NULL;
        size_t result_len = 0;
        microlisp_status st = microlisp_eval(state, buf, n, &result, &result_len);
        if (st == MICROLISP_OK) {
            if (result != NULL && result_len > 0) {
                if (fwrite(result, 1, result_len, out_file) != result_len ||
                    fputs_or_fail(out_file, "\n") != 0) {
                    microlisp_buffer_free(state, result);
                    return MICROLISP_ERR_IO;
                }
            }
            microlisp_buffer_free(state, result);
        } else {
            const char *msg = microlisp_state_error(state);
            const char *tag = microlisp_status_string(st);
            if (fprintf(out_file, "error (%s): %s\n", tag, msg) < 0) {
                return MICROLISP_ERR_IO;
            }
        }
    }
}
