/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for libmicrolisp. Uses a small in-file harness so the
 * project has no mandatory external test dependency.
 *
 * Each TEST() runs in isolation against a freshly-created
 * microlisp_state. CHECK logs a failure and continues; REQUIRE logs
 * and aborts the surrounding test body so a contract regression
 * produces a clean diagnostic instead of crashing the runner.
 */
/* strdup is POSIX, not C17. Must precede any system header. */
#if defined(__unix__) || defined(__APPLE__)
#ifndef _POSIX_C_SOURCE
/* NOLINTNEXTLINE(readability-identifier-naming) */
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "microlisp/microlisp.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

#define REQUIRE(cond)                                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s:%d: %s (required; aborting test)\n", __FILE__, __LINE__,   \
                    #cond);                                                                        \
            ++g_failures;                                                                          \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST(name)                                                                                 \
    static void name(void);                                                                        \
    static void run_##name(void) {                                                                 \
        ++g_tests;                                                                                 \
        fprintf(stderr, "[ RUN  ] %s\n", #name);                                                   \
        int before = g_failures;                                                                   \
        name();                                                                                    \
        fprintf(stderr, "[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", #name);             \
    }                                                                                              \
    static void name(void)

/* --------------------------------------------------------------------------
 * Helpers.
 * -------------------------------------------------------------------------- */

/** Evaluate @p src in a fresh state. Compare the printed result against
 *  @p expected (NULL means "no result" / unspecified). Returns 1 on
 *  match. */
static int eval_equals(const char *src, const char *expected) {
    microlisp_state *s = NULL;
    microlisp_status st = microlisp_state_create(NULL, &s);
    if (st != MICROLISP_OK) {
        fprintf(stderr, "  state_create failed: %s\n", microlisp_status_string(st));
        return 0;
    }
    char *result = NULL;
    size_t result_len = 0;
    st = microlisp_eval(s, src, strlen(src), &result, &result_len);
    int ok;
    if (st != MICROLISP_OK) {
        fprintf(stderr, "  eval `%s` failed: %s: %s\n", src, microlisp_status_string(st),
                microlisp_state_error(s));
        ok = 0;
    } else if (expected == NULL) {
        ok = (result == NULL || result_len == 0);
        if (!ok) {
            fprintf(stderr, "  expected unspecified, got `%s`\n", result);
        }
    } else if (result == NULL) {
        fprintf(stderr, "  expected `%s`, got unspecified\n", expected);
        ok = 0;
    } else {
        ok = strcmp(result, expected) == 0;
        if (!ok) {
            fprintf(stderr, "  expected `%s`, got `%s`\n", expected, result);
        }
    }
    microlisp_buffer_free(s, result);
    microlisp_state_destroy(s);
    return ok;
}

/** Evaluate @p src in a fresh state; expect @p want_status. Returns 1 on
 *  match. */
static int eval_fails_with(const char *src, microlisp_status want_status) {
    microlisp_state *s = NULL;
    microlisp_status st = microlisp_state_create(NULL, &s);
    if (st != MICROLISP_OK) {
        return 0;
    }
    char *result = NULL;
    size_t result_len = 0;
    st = microlisp_eval(s, src, strlen(src), &result, &result_len);
    int ok = (st == want_status);
    if (!ok) {
        fprintf(stderr, "  `%s`: expected %s, got %s (msg=%s)\n", src,
                microlisp_status_string(want_status), microlisp_status_string(st),
                microlisp_state_error(s));
    }
    microlisp_buffer_free(s, result);
    microlisp_state_destroy(s);
    return ok;
}

/* --------------------------------------------------------------------------
 * Reader tests.
 * -------------------------------------------------------------------------- */

TEST(integer_literals) {
    CHECK(eval_equals("0", "0"));
    CHECK(eval_equals("42", "42"));
    CHECK(eval_equals("-7", "-7"));
    CHECK(eval_equals("+13", "13"));
    /* 2^59 - 1 is the largest fixnum. */
    CHECK(eval_equals("576460752303423487", "576460752303423487"));
    CHECK(eval_equals("-576460752303423488", "-576460752303423488"));
}

TEST(integer_overflow_rejected) {
    CHECK(eval_fails_with("999999999999999999999", MICROLISP_ERR_OVERFLOW));
    /* 2^59 = 576460752303423488 -- one past the max. */
    CHECK(eval_fails_with("576460752303423488", MICROLISP_ERR_OVERFLOW));
}

TEST(boolean_literals) {
    CHECK(eval_equals("#t", "#t"));
    CHECK(eval_equals("#f", "#f"));
}

TEST(string_literals_and_escapes) {
    CHECK(eval_equals("\"hello\"", "\"hello\""));
    CHECK(eval_equals("\"line\\nbreak\"", "\"line\\nbreak\""));
    CHECK(eval_equals("\"quote\\\"inside\"", "\"quote\\\"inside\""));
    CHECK(eval_equals("\"\"", "\"\""));
    CHECK(eval_equals("\"hex\\x41\\x42\"", "\"hexAB\""));
}

TEST(comments_are_skipped) {
    CHECK(eval_equals("; first comment\n42 ; trailing\n", "42"));
}

TEST(quote_shorthand) {
    CHECK(eval_equals("'a", "a"));
    CHECK(eval_equals("'(1 2 3)", "(1 2 3)"));
    CHECK(eval_equals("'()", "()"));
}

TEST(dotted_pair) {
    CHECK(eval_equals("(quote (1 . 2))", "(1 . 2)"));
    CHECK(eval_equals("(quote (1 2 . 3))", "(1 2 . 3)"));
}

TEST(unmatched_parens) {
    CHECK(eval_fails_with("(a b", MICROLISP_ERR_READ_TRUNCATED));
    CHECK(eval_fails_with(")", MICROLISP_ERR_READ_SYNTAX));
}

TEST(deep_nesting_rejected) {
    /* The default max is 256; 600 opens should trip the limit. */
    size_t depth = 600;
    char *buf = (char *)malloc(depth + 1);
    REQUIRE(buf != NULL);
    memset(buf, '(', depth);
    buf[depth] = '\0';
    CHECK(eval_fails_with(buf, MICROLISP_ERR_READ_DEPTH));
    free(buf);
}

/* --------------------------------------------------------------------------
 * Arithmetic.
 * -------------------------------------------------------------------------- */

TEST(arithmetic_basic) {
    CHECK(eval_equals("(+ 1 2 3)", "6"));
    CHECK(eval_equals("(+)", "0"));
    CHECK(eval_equals("(- 10 3 2)", "5"));
    CHECK(eval_equals("(- 5)", "-5"));
    CHECK(eval_equals("(* 2 3 4)", "24"));
    CHECK(eval_equals("(*)", "1"));
    CHECK(eval_equals("(/ 20 4)", "5"));
    CHECK(eval_equals("(quotient 7 2)", "3"));
    CHECK(eval_equals("(remainder 7 2)", "1"));
    CHECK(eval_equals("(remainder -7 2)", "-1"));
    CHECK(eval_equals("(modulo -7 2)", "1"));
    CHECK(eval_equals("(modulo 7 -2)", "-1"));
}

TEST(arithmetic_overflow) {
    /* Use literals near the fixnum boundary to drive operation overflow. */
    CHECK(eval_fails_with("(+ 576460752303423487 1)", MICROLISP_ERR_OVERFLOW));
    CHECK(eval_fails_with("(* 1000000000 1000000000)", MICROLISP_ERR_OVERFLOW));
    CHECK(eval_fails_with("(- -576460752303423488 1)", MICROLISP_ERR_OVERFLOW));
}

TEST(division_by_zero) {
    CHECK(eval_fails_with("(/ 1 0)", MICROLISP_ERR_DIV_ZERO));
    CHECK(eval_fails_with("(quotient 1 0)", MICROLISP_ERR_DIV_ZERO));
    CHECK(eval_fails_with("(modulo 1 0)", MICROLISP_ERR_DIV_ZERO));
}

TEST(comparison_chained) {
    CHECK(eval_equals("(= 1 1 1)", "#t"));
    CHECK(eval_equals("(= 1 1 2)", "#f"));
    CHECK(eval_equals("(< 1 2 3)", "#t"));
    CHECK(eval_equals("(< 1 2 2)", "#f"));
    CHECK(eval_equals("(<= 1 2 2)", "#t"));
    CHECK(eval_equals("(>= 3 2 2)", "#t"));
    CHECK(eval_equals("(> 3 2 1)", "#t"));
}

/* --------------------------------------------------------------------------
 * Conditionals.
 * -------------------------------------------------------------------------- */

TEST(if_form) {
    CHECK(eval_equals("(if #t 1 2)", "1"));
    CHECK(eval_equals("(if #f 1 2)", "2"));
    CHECK(eval_equals("(if 0 1 2)", "1")); /* only #f is falsy */
    CHECK(eval_equals("(if (quote ()) 1 2)", "1"));
    CHECK(eval_equals("(if #f 1)", NULL));
}

TEST(cond_form) {
    CHECK(eval_equals("(cond ((= 1 2) 'a) ((= 2 2) 'b) (else 'c))", "b"));
    CHECK(eval_equals("(cond ((= 1 2) 'a) (else 'c))", "c"));
    CHECK(eval_equals("(cond ((= 1 2) 'a))", NULL));
    /* `(cond (test))`: yield the test value when it's truthy. */
    CHECK(eval_equals("(cond (42))", "42"));
}

TEST(and_or_short_circuit) {
    CHECK(eval_equals("(and)", "#t"));
    CHECK(eval_equals("(or)", "#f"));
    CHECK(eval_equals("(and 1 2 3)", "3"));
    CHECK(eval_equals("(and 1 #f 3)", "#f"));
    CHECK(eval_equals("(or #f #f 7)", "7"));
    CHECK(eval_equals("(or #f #f)", "#f"));
}

/* --------------------------------------------------------------------------
 * Lambda / closures.
 * -------------------------------------------------------------------------- */

TEST(lambda_basic) {
    CHECK(eval_equals("((lambda (x) (* x x)) 5)", "25"));
    CHECK(eval_equals("((lambda () 42))", "42"));
}

TEST(closures_capture_environment) {
    CHECK(eval_equals("(define (make-adder n) (lambda (x) (+ x n)))"
                      "(define add5 (make-adder 5))"
                      "(add5 100)",
                      "105"));
}

TEST(closures_with_mutable_state) {
    CHECK(eval_equals("(define (make-counter)"
                      "  (let ((n 0))"
                      "    (lambda () (set! n (+ n 1)) n)))"
                      "(define c (make-counter))"
                      "(c) (c) (c)",
                      "3"));
}

TEST(lambda_variadic) {
    CHECK(eval_equals("((lambda args args) 1 2 3)", "(1 2 3)"));
    CHECK(eval_equals("((lambda (x . rest) rest) 1 2 3)", "(2 3)"));
    CHECK(eval_equals("((lambda (x . rest) x) 1 2 3)", "1"));
    CHECK(eval_equals("((lambda (x . rest) rest) 1)", "()"));
}

TEST(arity_errors) {
    CHECK(eval_fails_with("((lambda (x y) x) 1)", MICROLISP_ERR_ARITY));
    CHECK(eval_fails_with("((lambda (x y) x) 1 2 3)", MICROLISP_ERR_ARITY));
    CHECK(eval_fails_with("(car)", MICROLISP_ERR_ARITY));
    CHECK(eval_fails_with("(cons 1)", MICROLISP_ERR_ARITY));
}

/* --------------------------------------------------------------------------
 * let / let* / letrec.
 * -------------------------------------------------------------------------- */

TEST(let_form) {
    CHECK(eval_equals("(let ((x 1) (y 2)) (+ x y))", "3"));
    /* let bindings are not visible to each other. */
    CHECK(eval_fails_with("(let ((x 1) (y x)) y)", MICROLISP_ERR_UNBOUND));
}

TEST(let_star_form) {
    CHECK(eval_equals("(let* ((x 1) (y (+ x 1))) y)", "2"));
}

TEST(letrec_form) {
    CHECK(eval_equals("(letrec ((even? (lambda (n) (if (= n 0) #t (odd? (- n 1)))))"
                      "         (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))"
                      "  (even? 50))",
                      "#t"));
}

/* --------------------------------------------------------------------------
 * Pairs, lists, predicates.
 * -------------------------------------------------------------------------- */

TEST(pair_operations) {
    CHECK(eval_equals("(cons 1 2)", "(1 . 2)"));
    CHECK(eval_equals("(cons 1 (cons 2 (cons 3 '())))", "(1 2 3)"));
    CHECK(eval_equals("(car '(1 2 3))", "1"));
    CHECK(eval_equals("(cdr '(1 2 3))", "(2 3)"));
    CHECK(eval_equals("(list 1 2 3)", "(1 2 3)"));
    CHECK(eval_equals("(length '(a b c d))", "4"));
    CHECK(eval_equals("(length '())", "0"));
}

TEST(predicates) {
    CHECK(eval_equals("(null? '())", "#t"));
    CHECK(eval_equals("(null? '(1))", "#f"));
    CHECK(eval_equals("(pair? '(1))", "#t"));
    CHECK(eval_equals("(pair? '())", "#f"));
    CHECK(eval_equals("(number? 42)", "#t"));
    CHECK(eval_equals("(number? \"x\")", "#f"));
    CHECK(eval_equals("(symbol? 'a)", "#t"));
    CHECK(eval_equals("(string? \"x\")", "#t"));
    CHECK(eval_equals("(boolean? #t)", "#t"));
    CHECK(eval_equals("(boolean? 0)", "#f"));
    CHECK(eval_equals("(procedure? car)", "#t"));
    CHECK(eval_equals("(procedure? (lambda (x) x))", "#t"));
}

TEST(equality) {
    CHECK(eval_equals("(eq? 'a 'a)", "#t"));
    CHECK(eval_equals("(eq? 'a 'b)", "#f"));
    CHECK(eval_equals("(eq? 42 42)", "#t"));
    CHECK(eval_equals("(eqv? \"abc\" \"abc\")", "#t"));
    CHECK(eval_equals("(equal? '(1 2 3) '(1 2 3))", "#t"));
    CHECK(eval_equals("(equal? '(1 (2 3)) '(1 (2 3)))", "#t"));
    CHECK(eval_equals("(equal? '(1 2 3) '(1 2 4))", "#f"));
}

/* --------------------------------------------------------------------------
 * Error paths.
 * -------------------------------------------------------------------------- */

TEST(type_errors) {
    CHECK(eval_fails_with("(car 5)", MICROLISP_ERR_TYPE));
    CHECK(eval_fails_with("(cdr 'a)", MICROLISP_ERR_TYPE));
    CHECK(eval_fails_with("(+ 1 \"two\")", MICROLISP_ERR_TYPE));
    CHECK(eval_fails_with("(5 1 2)", MICROLISP_ERR_TYPE));
}

TEST(unbound_variable) {
    CHECK(eval_fails_with("undefined-name", MICROLISP_ERR_UNBOUND));
}

TEST(user_error_primitive) {
    CHECK(eval_fails_with("(error \"oh no\")", MICROLISP_ERR_USER));
}

/* --------------------------------------------------------------------------
 * TCO + GC stress.
 * -------------------------------------------------------------------------- */

TEST(tail_call_optimization) {
    /* A non-TCO interpreter would overflow the C stack here. 200k is
     * well past anything implementable with naive recursion. */
    CHECK(eval_equals("(define (loop n) (if (= n 0) 'done (loop (- n 1))))"
                      "(loop 200000)",
                      "done"));
}

TEST(gc_stress_via_allocations) {
    /* Build and discard many cons cells; the GC must reclaim them and
     * the live root (the accumulating list) must survive every cycle.
     * Uses an accumulator so the recursion is in tail position; a
     * non-tail definition would hit MICROLISP_ERR_EVAL_DEPTH long
     * before we got a chance to stress the GC. */
    CHECK(eval_equals("(define (range-iter lo hi acc)"
                      "  (if (>= lo hi) acc"
                      "      (range-iter lo (- hi 1) (cons (- hi 1) acc))))"
                      "(length (range-iter 0 5000 (quote ())))",
                      "5000"));
}

TEST(eval_depth_limit_rejects_deep_non_tail_recursion) {
    /* Non-tail recursion that exceeds the default 1024-frame ceiling.
     * `(cons n (f (- n 1)))` -- the cons forces a frame per call. */
    CHECK(eval_fails_with("(define (f n) (if (= n 0) (quote ()) (cons n (f (- n 1))))) (f 2000)",
                          MICROLISP_ERR_EVAL_DEPTH));
}

TEST(gc_keeps_closure_captures_alive) {
    /* The captured env (with `n` bound) must survive collections caused
     * by allocation inside `(c)` over many iterations. */
    CHECK(eval_equals("(define (make-counter)"
                      "  (let ((n 0))"
                      "    (lambda () (set! n (+ n 1)) n)))"
                      "(define c (make-counter))"
                      /* The body allocates cons cells inside the loop to force
                       * collections; the captured `n` must survive every one. */
                      "(define (run k)"
                      "  (if (= k 0) (c)"
                      "      (begin (c)"
                      "             (cons k (cons k (cons k (quote ()))))"
                      "             (run (- k 1)))))"
                      "(run 5000)",
                      "5001"));
}

/* --------------------------------------------------------------------------
 * Read-write roundtrip property test.
 * -------------------------------------------------------------------------- */

/* Read `s`, write the resulting value with write-style, eval the
 * printed string, write THAT, and check the two prints match. Eval is
 * used as the parser here for simplicity: (quote ...) on the source. */
static int roundtrip_via_quote(const char *src) {
    char wrapped[1024];
    int n = snprintf(wrapped, sizeof wrapped, "(quote %s)", src);
    if (n < 0 || (size_t)n >= sizeof wrapped) {
        return 0;
    }
    microlisp_state *s = NULL;
    if (microlisp_state_create(NULL, &s) != MICROLISP_OK) {
        return 0;
    }
    char *r1 = NULL;
    size_t l1 = 0;
    microlisp_status st = microlisp_eval(s, wrapped, strlen(wrapped), &r1, &l1);
    if (st != MICROLISP_OK || r1 == NULL) {
        microlisp_state_destroy(s);
        return 0;
    }
    /* Now wrap r1 in another quote and eval. */
    char wrapped2[2048];
    n = snprintf(wrapped2, sizeof wrapped2, "(quote %s)", r1);
    if (n < 0 || (size_t)n >= sizeof wrapped2) {
        microlisp_buffer_free(s, r1);
        microlisp_state_destroy(s);
        return 0;
    }
    char *r2 = NULL;
    size_t l2 = 0;
    st = microlisp_eval(s, wrapped2, strlen(wrapped2), &r2, &l2);
    int ok = (st == MICROLISP_OK && r2 != NULL && strcmp(r1, r2) == 0);
    if (!ok) {
        fprintf(stderr, "  roundtrip(%s): r1=`%s` r2=`%s` st=%s\n", src, r1 ? r1 : "(null)",
                r2 ? r2 : "(null)", microlisp_status_string(st));
    }
    microlisp_buffer_free(s, r1);
    microlisp_buffer_free(s, r2);
    microlisp_state_destroy(s);
    return ok;
}

TEST(read_write_roundtrip) {
    CHECK(roundtrip_via_quote("42"));
    CHECK(roundtrip_via_quote("-7"));
    CHECK(roundtrip_via_quote("#t"));
    CHECK(roundtrip_via_quote("()"));
    CHECK(roundtrip_via_quote("(1 2 3)"));
    CHECK(roundtrip_via_quote("(1 (2 (3 (4))))"));
    CHECK(roundtrip_via_quote("(1 . 2)"));
    CHECK(roundtrip_via_quote("(a b . c)"));
    CHECK(roundtrip_via_quote("\"hello world\""));
    CHECK(roundtrip_via_quote("\"line\\nbreak\""));
}

/* --------------------------------------------------------------------------
 * Misc API.
 * -------------------------------------------------------------------------- */

TEST(empty_source_is_ok) {
    CHECK(eval_equals("", NULL));
    CHECK(eval_equals("   \n\t  ", NULL));
    CHECK(eval_equals("; only a comment\n", NULL));
}

TEST(version_string_is_nonempty) {
    const char *v = microlisp_version();
    REQUIRE(v != NULL);
    CHECK(v[0] != '\0');
}

TEST(status_string_for_every_code) {
    /* Every named status should have a non-default string. */
    microlisp_status codes[] = {
        MICROLISP_OK,
        MICROLISP_ERR_INVALID_ARG,
        MICROLISP_ERR_NOMEM,
        MICROLISP_ERR_READ_SYNTAX,
        MICROLISP_ERR_READ_TRUNCATED,
        MICROLISP_ERR_READ_DEPTH,
        MICROLISP_ERR_UNBOUND,
        MICROLISP_ERR_TYPE,
        MICROLISP_ERR_ARITY,
        MICROLISP_ERR_DIV_ZERO,
        MICROLISP_ERR_OVERFLOW,
        MICROLISP_ERR_USER,
        MICROLISP_ERR_IO,
        MICROLISP_ERR_SYNTAX,
        MICROLISP_ERR_EVAL_DEPTH,
    };
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        const char *s = microlisp_status_string(codes[i]);
        REQUIRE(s != NULL);
        CHECK(strcmp(s, "MICROLISP_ERR_UNKNOWN") != 0);
    }
}

TEST(unknown_status_yields_named_fallback) {
    CHECK(strcmp(microlisp_status_string((microlisp_status)9999), "MICROLISP_ERR_UNKNOWN") == 0);
}

TEST(allocator_partial_table_rejected) {
    microlisp_allocator a = {.alloc = NULL, .free = NULL, .user = NULL};
    /* alloc=NULL is partial; should reject. */
    microlisp_options opts = {.allocator = &a};
    microlisp_state *s = NULL;
    microlisp_status st = microlisp_state_create(&opts, &s);
    CHECK(st == MICROLISP_ERR_INVALID_ARG);
    CHECK(s == NULL);
}

/* --------------------------------------------------------------------------
 * Runner.
 * -------------------------------------------------------------------------- */

#define RUN(name) run_##name()

int main(void) {
    RUN(integer_literals);
    RUN(integer_overflow_rejected);
    RUN(boolean_literals);
    RUN(string_literals_and_escapes);
    RUN(comments_are_skipped);
    RUN(quote_shorthand);
    RUN(dotted_pair);
    RUN(unmatched_parens);
    RUN(deep_nesting_rejected);
    RUN(arithmetic_basic);
    RUN(arithmetic_overflow);
    RUN(division_by_zero);
    RUN(comparison_chained);
    RUN(if_form);
    RUN(cond_form);
    RUN(and_or_short_circuit);
    RUN(lambda_basic);
    RUN(closures_capture_environment);
    RUN(closures_with_mutable_state);
    RUN(lambda_variadic);
    RUN(arity_errors);
    RUN(let_form);
    RUN(let_star_form);
    RUN(letrec_form);
    RUN(pair_operations);
    RUN(predicates);
    RUN(equality);
    RUN(type_errors);
    RUN(unbound_variable);
    RUN(user_error_primitive);
    RUN(tail_call_optimization);
    RUN(gc_stress_via_allocations);
    RUN(eval_depth_limit_rejects_deep_non_tail_recursion);
    RUN(gc_keeps_closure_captures_alive);
    RUN(read_write_roundtrip);
    RUN(empty_source_is_ok);
    RUN(version_string_is_nonempty);
    RUN(status_string_for_every_code);
    RUN(unknown_status_yields_named_fallback);
    RUN(allocator_partial_table_rejected);

    fprintf(stderr, "\n%d test%s, %d failure%s\n", g_tests, g_tests == 1 ? "" : "s", g_failures,
            g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
