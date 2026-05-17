/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * Value printer.
 *
 * Two styles:
 *   - display (write_style=0): human-friendly. Strings unquoted, symbols
 *     bare.
 *   - write   (write_style=1): read-back-safe. Strings quoted with
 *     escapes; the output of (write v) re-parses to (equal?) v.
 *
 * Both styles render proper lists as `(a b c)` and improper as
 * `(a . b)` and mixed as `(a b . c)`. NIL prints as `()`. Closures
 * and primitives render as `#<procedure>` or `#<procedure:name>`.
 *
 * The walker is recursive but bounded by the reader's depth limit on
 * the constructed side. For user-built (set!-via-cons) structures it
 * relies on the GC's heap-object cap to constrain depth. A cycle in
 * a user-built structure WOULD cause unbounded recursion here; a
 * future v0.2 enhancement is to detect cycles via a small visited
 * set. v0.1 documents the limitation and falsifies it in the test
 * suite.
 */

#include "microlisp_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Sink helpers.
 * -------------------------------------------------------------------------- */

static microlisp_status emit(ml_out_fn out, void *user, const char *bytes, size_t len) {
    if (len == 0) {
        return MICROLISP_OK;
    }
    return out(bytes, len, user);
}

static microlisp_status emit_cstr(ml_out_fn out, void *user, const char *s) {
    return emit(out, user, s, strlen(s));
}

static microlisp_status emit_int(ml_out_fn out, void *user, int64_t v) {
    /* INT64_MIN prints as "-9223372036854775808" (20 chars). */
    char buf[24];
    int n = snprintf(buf, sizeof buf, "%" PRId64, v);
    if (n < 0 || (size_t)n >= sizeof buf) {
        return MICROLISP_ERR_IO;
    }
    return emit(out, user, buf, (size_t)n);
}

static microlisp_status emit_string_quoted(ml_out_fn out, void *user, const uint8_t *bytes,
                                           size_t len) {
    microlisp_status st = emit(out, user, "\"", 1);
    if (st != MICROLISP_OK) {
        return st;
    }
    /* Scan a contiguous safe run, flush, then handle the escape, repeat. */
    size_t run = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = bytes[i];
        if (c == '"' || c == '\\' || c < 0x20 || c == 0x7f) {
            if (run > 0) {
                st = emit(out, user, (const char *)(bytes + i - run), run);
                if (st != MICROLISP_OK) {
                    return st;
                }
                run = 0;
            }
            switch (c) {
            case '"':
                st = emit(out, user, "\\\"", 2);
                break;
            case '\\':
                st = emit(out, user, "\\\\", 2);
                break;
            case '\n':
                st = emit(out, user, "\\n", 2);
                break;
            case '\t':
                st = emit(out, user, "\\t", 2);
                break;
            case '\r':
                st = emit(out, user, "\\r", 2);
                break;
            case '\0':
                st = emit(out, user, "\\0", 2);
                break;
            default: {
                char buf[5];
                int n = snprintf(buf, sizeof buf, "\\x%02X", c);
                if (n < 0 || (size_t)n >= sizeof buf) {
                    return MICROLISP_ERR_IO;
                }
                st = emit(out, user, buf, (size_t)n);
                break;
            }
            }
            if (st != MICROLISP_OK) {
                return st;
            }
        } else {
            run++;
        }
    }
    if (run > 0) {
        st = emit(out, user, (const char *)(bytes + len - run), run);
        if (st != MICROLISP_OK) {
            return st;
        }
    }
    return emit(out, user, "\"", 1);
}

/* --------------------------------------------------------------------------
 * Recursive walker.
 * -------------------------------------------------------------------------- */

static microlisp_status print_value(ml_state *s, mvalue v, int write_style, ml_out_fn out,
                                    void *user);

static microlisp_status print_pair_contents(ml_state *s, mvalue v, int write_style, ml_out_fn out,
                                            void *user) {
    /* On entry, v is a pair. Print the chain. */
    microlisp_status st = print_value(s, ml_as_pair(v)->car, write_style, out, user);
    if (st != MICROLISP_OK) {
        return st;
    }
    mvalue rest = ml_as_pair(v)->cdr;
    while (ml_is_pair(rest)) {
        st = emit(out, user, " ", 1);
        if (st != MICROLISP_OK) {
            return st;
        }
        st = print_value(s, ml_as_pair(rest)->car, write_style, out, user);
        if (st != MICROLISP_OK) {
            return st;
        }
        rest = ml_as_pair(rest)->cdr;
    }
    if (rest != MV_NIL) {
        st = emit(out, user, " . ", 3);
        if (st != MICROLISP_OK) {
            return st;
        }
        st = print_value(s, rest, write_style, out, user);
        if (st != MICROLISP_OK) {
            return st;
        }
    }
    return MICROLISP_OK;
}

static microlisp_status print_value(ml_state *s, mvalue v, int write_style, ml_out_fn out,
                                    void *user) {
    if (ml_is_fix(v)) {
        return emit_int(out, user, ml_fix_int(v));
    }
    if (v == MV_NIL) {
        return emit(out, user, "()", 2);
    }
    if (v == MV_TRUE) {
        return emit(out, user, "#t", 2);
    }
    if (v == MV_FALSE) {
        return emit(out, user, "#f", 2);
    }
    if (v == MV_EOF) {
        return emit_cstr(out, user, "#<eof>");
    }
    if (v == MV_UNDEF) {
        return emit_cstr(out, user, "#<undefined>");
    }
    if (ml_is_sym(v)) {
        size_t len;
        const char *name = ml_sym_name(s, v, &len);
        return emit(out, user, name, len);
    }
    if (ml_is_string(v)) {
        mstring *m = ml_as_string(v);
        if (write_style) {
            return emit_string_quoted(out, user, m->bytes, m->len);
        }
        return emit(out, user, (const char *)m->bytes, m->len);
    }
    if (ml_is_pair(v)) {
        microlisp_status st = emit(out, user, "(", 1);
        if (st != MICROLISP_OK) {
            return st;
        }
        st = print_pair_contents(s, v, write_style, out, user);
        if (st != MICROLISP_OK) {
            return st;
        }
        return emit(out, user, ")", 1);
    }
    if (ml_is_prim(v)) {
        mprim *p = ml_as_prim(v);
        char buf[128];
        int n = snprintf(buf, sizeof buf, "#<procedure:%s>", p->name);
        if (n < 0) {
            return MICROLISP_ERR_IO;
        }
        size_t plen = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
        return emit(out, user, buf, plen);
    }
    if (ml_is_closure(v)) {
        return emit_cstr(out, user, "#<procedure>");
    }
    if (ml_is_env(v)) {
        return emit_cstr(out, user, "#<environment>");
    }
    return emit_cstr(out, user, "#<unknown>");
}

microlisp_status ml_print(ml_state *s, mvalue v, int write_style, ml_out_fn out, void *user) {
    return print_value(s, v, write_style, out, user);
}

/* --------------------------------------------------------------------------
 * Print-to-allocated-buffer convenience.
 * -------------------------------------------------------------------------- */

typedef struct print_buf {
    ml_state *s;
    uint8_t *bytes;
    size_t len;
    size_t cap;
} print_buf;

static microlisp_status print_buf_sink(const void *bytes, size_t len, void *user) {
    print_buf *pb = (print_buf *)user;
    if (len > SIZE_MAX - pb->len - 1) {
        return MICROLISP_ERR_NOMEM;
    }
    size_t need = pb->len + len + 1; /* +1 for trailing NUL */
    if (need > pb->cap) {
        size_t new_cap = pb->cap == 0 ? 64 : pb->cap;
        while (new_cap < need) {
            size_t doubled;
            if (__builtin_mul_overflow(new_cap, (size_t)2, &doubled)) {
                return MICROLISP_ERR_NOMEM;
            }
            new_cap = doubled;
        }
        uint8_t *grown = (uint8_t *)ml_raw_alloc(pb->s, new_cap);
        if (grown == NULL) {
            return MICROLISP_ERR_NOMEM;
        }
        if (pb->bytes != NULL) {
            memcpy(grown, pb->bytes, pb->len);
            ml_raw_free(pb->s, pb->bytes);
        }
        pb->bytes = grown;
        pb->cap = new_cap;
    }
    memcpy(pb->bytes + pb->len, bytes, len);
    pb->len += len;
    return MICROLISP_OK;
}

microlisp_status ml_print_to_alloc(ml_state *s, mvalue v, int write_style, char **out_bytes,
                                   size_t *out_len) {
    print_buf pb = {.s = s, .bytes = NULL, .len = 0, .cap = 0};
    microlisp_status st = ml_print(s, v, write_style, print_buf_sink, &pb);
    if (st != MICROLISP_OK) {
        ml_raw_free(s, pb.bytes);
        return st;
    }
    /* Ensure room for the NUL terminator. */
    if (pb.cap == 0) {
        pb.bytes = (uint8_t *)ml_raw_alloc(s, 1);
        if (pb.bytes == NULL) {
            return MICROLISP_ERR_NOMEM;
        }
        pb.cap = 1;
    }
    pb.bytes[pb.len] = '\0';
    *out_bytes = (char *)pb.bytes;
    if (out_len != NULL) {
        *out_len = pb.len;
    }
    return MICROLISP_OK;
}
