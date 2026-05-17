/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * Reader: turn a byte buffer into an S-expression tree.
 *
 * Tokenization is integrated with the parser -- there is no separate
 * token stream. The reader function peeks at the next non-whitespace
 * byte and dispatches into the appropriate sub-reader.
 *
 * Memory discipline: every fresh heap allocation is either tied
 * immediately into a protected parent (list-building uses a dummy
 * head pair held on the protect stack) or pushed onto the protect
 * stack before the next allocation. The dummy-head idiom keeps the
 * traversal pointer (`tail`) reachable through the protected head.
 *
 * Numbers: 60-bit signed integers. Out-of-range literals produce
 * MICROLISP_ERR_OVERFLOW so the reader never silently wraps.
 *
 * Strings: support the conventional escapes \n \t \r \" \\ \0 and a
 * generic \xHH (two hex digits). NUL bytes inside strings are
 * preserved verbatim and survive into the string heap object.
 */

#include "microlisp_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Position bookkeeping.
 * -------------------------------------------------------------------------- */

static int peek(const ml_reader *r) {
    if (r->pos >= r->len) {
        return -1;
    }
    return r->input[r->pos];
}

static int advance(ml_reader *r) {
    if (r->pos >= r->len) {
        return -1;
    }
    int c = r->input[r->pos++];
    if (c == '\n') {
        r->line++;
        r->column = 1;
    } else {
        r->column++;
    }
    return c;
}

static int is_whitespace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int is_terminator(int c) {
    return c < 0 || is_whitespace(c) || c == '(' || c == ')' || c == ';' || c == '"' || c == '\'';
}

static int is_digit(int c) {
    return c >= '0' && c <= '9';
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + c - 'a';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + c - 'A';
    }
    return -1;
}

static void skip_ws_and_comments(ml_reader *r) {
    for (;;) {
        int c = peek(r);
        if (c < 0) {
            return;
        }
        if (is_whitespace(c)) {
            advance(r);
            continue;
        }
        if (c == ';') {
            /* Line comment to EOL. */
            while ((c = peek(r)) >= 0 && c != '\n') {
                advance(r);
            }
            continue;
        }
        return;
    }
}

/* --------------------------------------------------------------------------
 * Sub-readers.
 * -------------------------------------------------------------------------- */

static microlisp_status read_form(ml_state *s, ml_reader *r, mvalue *out);

static microlisp_status read_string(ml_state *s, ml_reader *r, mvalue *out) {
    /* Caller positioned us on the opening `"`. */
    size_t start_line = r->line;
    size_t start_column = r->column;
    advance(r); /* consume `"` */

    /* Collect into a heap-grown buffer. We allocate via the state
     * allocator (not GC) for the scratch buffer; once we know the
     * length we copy into a final GC-managed string and free the
     * scratch. */
    size_t cap = 32;
    size_t len = 0;
    uint8_t *buf = (uint8_t *)ml_raw_alloc(s, cap);
    if (buf == NULL) {
        return MICROLISP_ERR_NOMEM;
    }
    for (;;) {
        int c = peek(r);
        if (c < 0) {
            ml_raw_free(s, buf);
            ml_set_error(s, start_line, start_column, "unterminated string literal");
            return MICROLISP_ERR_READ_TRUNCATED;
        }
        advance(r);
        if (c == '"') {
            break;
        }
        if (c == '\\') {
            int esc = advance(r);
            if (esc < 0) {
                ml_raw_free(s, buf);
                ml_set_error(s, start_line, start_column, "unterminated escape sequence in string");
                return MICROLISP_ERR_READ_TRUNCATED;
            }
            switch (esc) {
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            case 'r':
                c = '\r';
                break;
            case '0':
                c = '\0';
                break;
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case 'x': {
                int hi = advance(r);
                int lo = advance(r);
                int h1 = hex_digit(hi);
                int h2 = hex_digit(lo);
                if (h1 < 0 || h2 < 0) {
                    ml_raw_free(s, buf);
                    ml_set_error(s, r->line, r->column,
                                 "malformed \\xHH escape: need two hex digits");
                    return MICROLISP_ERR_READ_SYNTAX;
                }
                c = (h1 << 4) | h2;
                break;
            }
            default:
                ml_raw_free(s, buf);
                ml_set_error(s, r->line, r->column, "unknown string escape \\%c", esc);
                return MICROLISP_ERR_READ_SYNTAX;
            }
        }
        if (len == cap) {
            size_t new_cap;
            if (__builtin_mul_overflow(cap, (size_t)2, &new_cap)) {
                ml_raw_free(s, buf);
                return MICROLISP_ERR_NOMEM;
            }
            uint8_t *grown = (uint8_t *)ml_raw_alloc(s, new_cap);
            if (grown == NULL) {
                ml_raw_free(s, buf);
                return MICROLISP_ERR_NOMEM;
            }
            memcpy(grown, buf, len);
            ml_raw_free(s, buf);
            buf = grown;
            cap = new_cap;
        }
        buf[len++] = (uint8_t)c;
    }

    microlisp_status st = ml_gc_alloc_string(s, buf, len, out);
    ml_raw_free(s, buf);
    return st;
}

static microlisp_status read_hash(ml_state *s, ml_reader *r, mvalue *out) {
    /* On entry we are positioned at `#`. v0.1 supports only `#t` and
     * `#f`. (Reader macros like `#(` for vectors are deferred.) */
    size_t line = r->line;
    size_t column = r->column;
    advance(r); /* consume `#` */
    int c = advance(r);
    int next = peek(r);
    if (c == 't' && is_terminator(next)) {
        *out = MV_TRUE;
        return MICROLISP_OK;
    }
    if (c == 'f' && is_terminator(next)) {
        *out = MV_FALSE;
        return MICROLISP_OK;
    }
    ml_set_error(s, line, column, "unknown # form (only #t and #f are supported)");
    return MICROLISP_ERR_READ_SYNTAX;
}

static microlisp_status read_number(ml_state *s, ml_reader *r, int sign, mvalue *out) {
    /* Caller has already consumed the sign (if any). r->pos points at
     * the first digit. */
    size_t start_line = r->line;
    size_t start_column = r->column;
    int64_t value = 0;
    int any_digit = 0;
    for (;;) {
        int c = peek(r);
        if (!is_digit(c)) {
            break;
        }
        advance(r);
        any_digit = 1;
        /* value = value * 10 + (c - '0') with overflow checks against
         * the fixnum range, not just int64. */
        int64_t digit = c - '0';
        int64_t multiplied;
        int64_t added;
        if (__builtin_mul_overflow(value, (int64_t)10, &multiplied) ||
            __builtin_add_overflow(multiplied, digit, &added)) {
            /* Past int64 overflow -- definitely past fixnum. Consume any
             * remaining digits so the reader stays anchored on a token
             * boundary, then report. */
            while (is_digit(peek(r))) {
                advance(r);
            }
            ml_set_error(s, start_line, start_column,
                         "integer literal exceeds 60-bit fixnum range");
            return MICROLISP_ERR_OVERFLOW;
        }
        value = added;
    }
    if (!any_digit) {
        /* Sign with no digits is the caller's bug -- should be parsed
         * as a symbol instead. */
        ml_set_error(s, start_line, start_column, "expected digit after sign");
        return MICROLISP_ERR_READ_SYNTAX;
    }
    if (!is_terminator(peek(r))) {
        ml_set_error(s, r->line, r->column, "integer literal not followed by a token terminator");
        return MICROLISP_ERR_READ_SYNTAX;
    }
    /* Apply sign. Negation can't overflow int64 here because `value` is
     * already within int64 from the unsigned-direction parse above. */
    if (sign < 0) {
        value = -value;
    }
    if (value < M_FIX_MIN || value > M_FIX_MAX) {
        ml_set_error(s, start_line, start_column, "integer literal exceeds 60-bit fixnum range");
        return MICROLISP_ERR_OVERFLOW;
    }
    *out = ml_make_fix(value);
    return MICROLISP_OK;
}

static microlisp_status read_symbol(ml_state *s, ml_reader *r, mvalue *out) {
    size_t start = r->pos;
    while (!is_terminator(peek(r))) {
        advance(r);
    }
    size_t len = r->pos - start;
    return ml_sym_intern(s, (const char *)(r->input + start), len, out);
}

/* List reader. On entry the caller has consumed the opening `(`. */
static microlisp_status read_list(ml_state *s, ml_reader *r, mvalue *out) {
    if (r->depth >= s->max_read_depth) {
        ml_set_error(s, r->line, r->column, "nesting depth exceeds limit of %zu",
                     s->max_read_depth);
        return MICROLISP_ERR_READ_DEPTH;
    }
    r->depth++;

    skip_ws_and_comments(r);
    if (peek(r) == ')') {
        advance(r);
        r->depth--;
        *out = MV_NIL;
        return MICROLISP_OK;
    }

    /* Dummy head: keep the in-flight list rooted via head_pair.cdr. */
    size_t sp = ml_gc_savepoint(s);
    mvalue head;
    microlisp_status st = ml_gc_alloc_pair(s, MV_NIL, MV_NIL, &head);
    if (st != MICROLISP_OK) {
        return st;
    }
    st = ml_gc_protect(s, head);
    if (st != MICROLISP_OK) {
        return st;
    }
    mvalue tail = head;

    int saw_dotted = 0;
    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c < 0) {
            ml_set_error(s, r->line, r->column, "unterminated list");
            ml_gc_restore(s, sp);
            return MICROLISP_ERR_READ_TRUNCATED;
        }
        if (c == ')') {
            advance(r);
            break;
        }
        /* Dotted pair: `.` followed by whitespace, then one more form,
         * then a `)`. */
        if (c == '.') {
            /* Only treat as dot if next byte is a terminator -- else
             * it's a symbol starting with `.`. */
            if (r->pos + 1 < r->len && is_terminator(r->input[r->pos + 1])) {
                advance(r); /* consume `.` */
                if (tail == head) {
                    ml_set_error(s, r->line, r->column, "dot at start of list");
                    ml_gc_restore(s, sp);
                    return MICROLISP_ERR_READ_SYNTAX;
                }
                mvalue cdr_form;
                st = read_form(s, r, &cdr_form);
                if (st != MICROLISP_OK) {
                    ml_gc_restore(s, sp);
                    return st;
                }
                /* Setting through the heap pointer; `tail` is reachable
                 * via head, so it survives any future collection. */
                ml_as_pair(tail)->cdr = cdr_form;
                saw_dotted = 1;
                skip_ws_and_comments(r);
                if (peek(r) != ')') {
                    ml_set_error(s, r->line, r->column, "expected `)` after dotted-pair tail form");
                    ml_gc_restore(s, sp);
                    return MICROLISP_ERR_READ_SYNTAX;
                }
                advance(r);
                break;
            }
            /* Fall through: bare `.` followed by more chars is a symbol. */
        }
        mvalue elem;
        st = read_form(s, r, &elem);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        /* Protect elem across the new-pair allocation. */
        st = ml_gc_protect(s, elem);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        mvalue cell;
        st = ml_gc_alloc_pair(s, elem, MV_NIL, &cell);
        ml_gc_unprotect(s, 1);
        if (st != MICROLISP_OK) {
            ml_gc_restore(s, sp);
            return st;
        }
        ml_as_pair(tail)->cdr = cell;
        tail = cell;
    }

    *out = ml_as_pair(head)->cdr;
    (void)saw_dotted;
    ml_gc_restore(s, sp);
    r->depth--;
    return MICROLISP_OK;
}

static microlisp_status read_quote(ml_state *s, ml_reader *r, mvalue *out) {
    size_t start_line = r->line;
    size_t start_column = r->column;
    advance(r); /* consume `'` */
    mvalue form;
    microlisp_status st = read_form(s, r, &form);
    if (st != MICROLISP_OK) {
        return st;
    }
    /* A lone `'` at end-of-input -- read_form returns MV_EOF rather than
     * an error, so we have to reject it explicitly here or the caller
     * would silently see `(quote #<eof>)`. */
    if (form == MV_EOF) {
        ml_set_error(s, start_line, start_column, "lone `'` with no following datum");
        return MICROLISP_ERR_READ_TRUNCATED;
    }
    /* Build `(quote <form>)` = cons(quote, cons(form, nil)). */
    size_t sp = ml_gc_savepoint(s);
    st = ml_gc_protect(s, form);
    if (st != MICROLISP_OK) {
        return st;
    }
    mvalue inner;
    st = ml_gc_alloc_pair(s, form, MV_NIL, &inner);
    if (st != MICROLISP_OK) {
        ml_gc_restore(s, sp);
        return st;
    }
    ml_gc_unprotect(s, 1); /* form is now reachable via inner */
    st = ml_gc_protect(s, inner);
    if (st != MICROLISP_OK) {
        return st;
    }
    st = ml_gc_alloc_pair(s, s->sym_quote, inner, out);
    ml_gc_restore(s, sp);
    return st;
}

static microlisp_status read_form(ml_state *s, ml_reader *r, mvalue *out) {
    skip_ws_and_comments(r);
    int c = peek(r);
    if (c < 0) {
        *out = MV_EOF;
        return MICROLISP_OK;
    }
    if (c == '(') {
        advance(r);
        return read_list(s, r, out);
    }
    if (c == ')') {
        ml_set_error(s, r->line, r->column, "unexpected `)`");
        return MICROLISP_ERR_READ_SYNTAX;
    }
    if (c == '\'') {
        return read_quote(s, r, out);
    }
    if (c == '"') {
        return read_string(s, r, out);
    }
    if (c == '#') {
        return read_hash(s, r, out);
    }
    if (is_digit(c)) {
        return read_number(s, r, /*sign=*/1, out);
    }
    if (c == '+' || c == '-') {
        /* Lookahead: digit → number; otherwise → symbol. */
        if (r->pos + 1 < r->len && is_digit(r->input[r->pos + 1])) {
            int sign = (c == '-') ? -1 : 1;
            advance(r);
            return read_number(s, r, sign, out);
        }
        return read_symbol(s, r, out);
    }
    return read_symbol(s, r, out);
}

microlisp_status ml_read(ml_state *s, ml_reader *r, mvalue *out) {
    return read_form(s, r, out);
}
