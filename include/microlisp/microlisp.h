/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * Public API for microlisp: a small Scheme-subset interpreter.
 *
 * The language is a strict R5RS-flavored Scheme subset:
 *
 *     Types       int64, boolean (#t/#f), symbol, string, pair, '(),
 *                 procedure (closure or built-in primitive)
 *     Forms       quote, if, define, lambda, set!, let, let*, letrec,
 *                 begin, and, or, cond
 *     Built-ins   arithmetic (+ - * / quotient remainder modulo)
 *                 comparison (= < > <= >=)
 *                 list ops   (cons car cdr list pair? null? length)
 *                 predicates (eq? eqv? equal? number? symbol?
 *                             string? procedure? boolean?)
 *                 I/O        (display newline write error)
 *
 * Out of scope for v0.1: floats, chars, vectors, macros, continuations,
 * exceptions (callers handle errors as status codes).
 *
 * The implementation uses tagged pointers (low-3-bit tag on uintptr_t)
 * for the value representation and a mark-and-sweep garbage collector
 * for heap-allocated objects. Proper tail-call optimization is provided
 * by a trampolined evaluator -- tail-recursive Scheme programs run in
 * O(1) C-stack space.
 *
 * @par Thread safety:
 *      Each ::microlisp_state owns its own GC heap, symbol table, and
 *      global environment. A given state is **MT-Unsafe**: all calls on
 *      the same state must be serialized by the caller. Different
 *      states may be used concurrently from different threads (every
 *      function is MT-Safe-per-instance).
 *
 * @par Memory:
 *      Allocation goes through an optional ::microlisp_allocator; pass
 *      NULL to use platform @c malloc / @c free. The allocator is used
 *      for both bookkeeping and GC-managed heap objects.
 *
 * @par Embedding model:
 *      The v0.1 API is intentionally minimal: callers supply Scheme
 *      source as a byte buffer, get back a printed result and a status
 *      code. A value-level C API (build values from C, call Scheme
 *      procedures with C-constructed arguments) is deferred to a
 *      future minor release once the internal representation is
 *      stable.
 */
#ifndef MICROLISP_MICROLISP_H
#define MICROLISP_MICROLISP_H

#include "microlisp/export.h"
#include "microlisp/version.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Types ------------------------------------------------------------------ */

/**
 * Error and status codes returned by every fallible API entry point.
 *
 * Callers that switch on this enum should include a `default:` arm to
 * handle codes added in future minor releases without breaking.
 */
typedef enum microlisp_status {
    /** Success. */
    MICROLISP_OK = 0,

    /** A required argument was NULL or otherwise structurally invalid. */
    MICROLISP_ERR_INVALID_ARG = 1,

    /** Allocator (or platform malloc) returned NULL. */
    MICROLISP_ERR_NOMEM = 2,

    /** Reader encountered a syntax error -- bad token, unmatched paren,
     *  malformed string escape, etc. */
    MICROLISP_ERR_READ_SYNTAX = 3,

    /** Reader reached end-of-input mid-form (e.g. `(a b` with no
     *  closing paren). */
    MICROLISP_ERR_READ_TRUNCATED = 4,

    /** Reader nesting depth exceeded the configured limit (DoS guard
     *  against pathologically deep inputs). */
    MICROLISP_ERR_READ_DEPTH = 5,

    /** Evaluator looked up a symbol that has no binding in any frame
     *  of the active environment. */
    MICROLISP_ERR_UNBOUND = 6,

    /** Evaluator received a value of the wrong type -- e.g. @c car of
     *  a non-pair, or @c + on a non-number. */
    MICROLISP_ERR_TYPE = 7,

    /** Procedure called with the wrong number of arguments. */
    MICROLISP_ERR_ARITY = 8,

    /** Division by zero in @c / , @c quotient , @c remainder , or
     *  @c modulo . */
    MICROLISP_ERR_DIV_ZERO = 9,

    /** Integer operation would overflow the @c int64_t range. */
    MICROLISP_ERR_OVERFLOW = 10,

    /** A Scheme program invoked the @c error built-in. The argument
     *  message is available via ::microlisp_state_error. */
    MICROLISP_ERR_USER = 11,

    /** An output stream reported a write error during @c display ,
     *  @c write , or @c newline , or while writing the result. */
    MICROLISP_ERR_IO = 12,

    /** Malformed special form -- e.g. @c (lambda) with no body or
     *  @c (define) with no name. */
    MICROLISP_ERR_SYNTAX = 13,

    /** Evaluator recursion depth exceeded the configured limit. Caused
     *  by deep non-tail-recursive Scheme code; tail-recursive code
     *  runs in constant C-stack space regardless of depth. Limits
     *  blast radius from untrusted input. */
    MICROLISP_ERR_EVAL_DEPTH = 14,

    /** Printer recursion depth exceeded the configured limit. Caused
     *  by deeply-nested-list output (e.g. an accumulator chain like
     *  `(nest n acc) -> (nest (- n 1) (list acc))` produces a list
     *  of depth N in the car direction). The printer walks pair-car
     *  chains recursively; this guard keeps the host C stack
     *  bounded. v0.2 will replace the recursive walker with an
     *  iterative one and lift the limit. */
    MICROLISP_ERR_PRINT_DEPTH = 15
} microlisp_status;

/** Opaque interpreter state. Construct with ::microlisp_state_create
 *  and destroy with ::microlisp_state_destroy. */
typedef struct microlisp_state microlisp_state;

/**
 * Caller-supplied allocator. Pass NULL to ::microlisp_state_create to
 * use the platform @c malloc / @c free.
 *
 * The implementation only calls @p alloc and @p free; it does not call
 * realloc, calloc, or memalign.
 *
 * @par Thread safety: the caller's @p alloc / @p free functions must
 *      themselves be MT-Safe if the @ref microlisp_allocator is shared
 *      across threads (i.e. used to construct multiple states that run
 *      concurrently).
 */
typedef struct microlisp_allocator {
    /** Allocate @p size bytes. Returns NULL on failure. Must be non-NULL
     *  whenever the surrounding ::microlisp_allocator struct is non-NULL. */
    void *(*alloc)(size_t size, void *user);
    /** Free a pointer previously returned by @p alloc. Must be non-NULL
     *  whenever the surrounding ::microlisp_allocator struct is non-NULL.
     *  @c free(NULL) is a no-op. */
    void (*free)(void *ptr, void *user);
    /** Opaque user data passed verbatim to @p alloc and @p free. */
    void *user;
} microlisp_allocator;

/** Default value for ::microlisp_options::max_read_depth. */
#define MICROLISP_DEFAULT_MAX_READ_DEPTH 256

/** Default value for ::microlisp_options::max_eval_depth. */
#define MICROLISP_DEFAULT_MAX_EVAL_DEPTH 1024

/** Default value for ::microlisp_options::max_print_depth. */
#define MICROLISP_DEFAULT_MAX_PRINT_DEPTH 1024

/**
 * Options controlling a state. Zero-initialize to use defaults; pass
 * NULL to ::microlisp_state_create for "all defaults."
 */
typedef struct microlisp_options {
    /** Allocator. NULL means stdlib @c malloc / @c free. */
    const microlisp_allocator *allocator;

    /** Maximum reader nesting depth. 0 means
     *  ::MICROLISP_DEFAULT_MAX_READ_DEPTH. Forms deeper than the limit
     *  fail with ::MICROLISP_ERR_READ_DEPTH. */
    size_t max_read_depth;

    /** Maximum evaluator recursion depth. 0 means
     *  ::MICROLISP_DEFAULT_MAX_EVAL_DEPTH. Tail-recursive code runs
     *  in constant C-stack space regardless of this limit; the cap
     *  only constrains *non-tail-recursive* invocations that grow
     *  the host C stack. Forms that exceed it fail with
     *  ::MICROLISP_ERR_EVAL_DEPTH. */
    size_t max_eval_depth;

    /** Maximum printer pair-walk depth. 0 means
     *  ::MICROLISP_DEFAULT_MAX_PRINT_DEPTH. Output of trees deeper
     *  than this fails with ::MICROLISP_ERR_PRINT_DEPTH instead of
     *  exhausting the host C stack. v0.1's printer is recursive on
     *  the pair-car axis; the limit bounds the worst case until
     *  v0.2's iterative printer lands. */
    size_t max_print_depth;

    /** Hard ceiling on heap objects between GC collections. 0 means
     *  "use the built-in default" (currently 4096 objects, doubling
     *  with the live set after each collection). This is a heuristic
     *  knob, not a contract: collection policy may change between
     *  minor releases. */
    size_t gc_initial_threshold;
} microlisp_options;

/* -- State lifecycle -------------------------------------------------------- */

/**
 * Create a fresh interpreter state with the built-in primitive bindings
 * loaded in the top-level environment.
 *
 * @param opts       Options. NULL means "all defaults."
 * @param[out] out_state Receives the new state on success. Set to NULL
 *                   on error. Caller owns it and must release with
 *                   ::microlisp_state_destroy.
 * @return ::MICROLISP_OK on success, ::MICROLISP_ERR_NOMEM if any
 *         setup allocation failed, ::MICROLISP_ERR_INVALID_ARG if
 *         @p out_state is NULL or @p opts has a partial allocator
 *         table.
 *
 * @par Thread safety: MT-Safe.
 */
MICROLISP_API MICROLISP_NODISCARD microlisp_status
microlisp_state_create(const microlisp_options *opts, microlisp_state **out_state);

/** Destroy a state, releasing its GC heap and all interned symbols.
 *  NULL is a no-op. */
MICROLISP_API void microlisp_state_destroy(microlisp_state *state);

/* -- Evaluation ------------------------------------------------------------- */

/**
 * Evaluate a source buffer of Scheme code as a sequence of top-level
 * forms.
 *
 * Each top-level form is read and evaluated in order; the result of
 * the **last** form is the result of the call. Side effects of earlier
 * forms (defines, set!, displays) persist in @p state.
 *
 * @param state       Interpreter state. Must not be NULL.
 * @param source      Scheme source bytes. May be NULL iff @p source_len
 *                    is 0 (in which case the call is a no-op returning
 *                    ::MICROLISP_OK with @p out_bytes set to NULL and
 *                    @p out_len set to 0).
 * @param source_len  Length of @p source in bytes.
 * @param[out] out_bytes
 *                    If non-NULL, receives a pointer to a freshly
 *                    allocated, NUL-terminated string containing the
 *                    printed result of the last form (in @c write
 *                    style: strings quoted, symbols bare). The buffer
 *                    is allocated with the state's allocator and must
 *                    be released with ::microlisp_buffer_free, passing
 *                    the same state. May be NULL if the caller doesn't
 *                    need the result text.
 * @param[out] out_len
 *                    If non-NULL, receives the length of @p *out_bytes
 *                    excluding the trailing NUL. May be NULL.
 *
 * @return ::MICROLISP_OK on success, or a specific error code.
 *
 * On any non-OK return, @p *out_bytes is set to NULL and @p *out_len
 * to 0; the human-readable error message is retrievable via
 * ::microlisp_state_error.
 *
 * @par Thread safety: MT-Unsafe. The caller must serialize all calls
 *      on the same @p state.
 */
MICROLISP_API MICROLISP_NODISCARD microlisp_status microlisp_eval(microlisp_state *state,
                                                                  const char *source,
                                                                  size_t source_len,
                                                                  char **out_bytes,
                                                                  size_t *out_len);

/**
 * Run an interactive Read-Eval-Print Loop reading from @p in_file and
 * writing prompts and results to @p out_file.
 *
 * Errors at any form abort that form's evaluation and print a
 * diagnostic to @p out_file, then the loop continues with the next
 * prompt. The function returns ::MICROLISP_OK when @p in_file reaches
 * end-of-file cleanly, or ::MICROLISP_ERR_IO if a write to @p out_file
 * fails.
 *
 * @param state    Interpreter state. Must not be NULL.
 * @param in_file  Input stream. NULL means @c stdin.
 * @param out_file Output stream for prompts, results, and diagnostics.
 *                 NULL means @c stdout.
 * @param prompt   Prompt string printed before each input line. NULL
 *                 means no prompt (suitable for piped input).
 *
 * @par Thread safety: MT-Unsafe (per state).
 */
MICROLISP_API MICROLISP_NODISCARD microlisp_status microlisp_repl(microlisp_state *state,
                                                                  FILE *in_file, FILE *out_file,
                                                                  const char *prompt);

/**
 * Free a buffer previously returned by ::microlisp_eval via
 * @p out_bytes . NULL is a no-op.
 *
 * @param state  The state that produced the buffer; the same allocator
 *               must be used to free it.
 * @param bytes  Buffer pointer.
 */
MICROLISP_API void microlisp_buffer_free(microlisp_state *state, char *bytes);

/* -- Diagnostics ------------------------------------------------------------ */

/**
 * Retrieve a human-readable description of the most recent error on
 * @p state. The pointer remains valid until the next call into
 * ::microlisp_eval on the same state, or until ::microlisp_state_destroy.
 *
 * For ::MICROLISP_OK the result is the empty string. Never returns
 * NULL.
 *
 * @par Thread safety: MT-Unsafe (per state).
 */
MICROLISP_API const char *microlisp_state_error(const microlisp_state *state);

/**
 * Source position of the most recent error, as @p line and @p column
 * (both 1-based). Reports `0, 0` if the error has no associated
 * position (e.g. ::MICROLISP_ERR_NOMEM) or if @p state is NULL.
 *
 * @par Thread safety: MT-Unsafe (per state).
 */
MICROLISP_API void microlisp_state_error_position(const microlisp_state *state, size_t *out_line,
                                                  size_t *out_column);

/* -- Misc ------------------------------------------------------------------- */

/** Library version (e.g. "0.1.0"). Never NULL. */
MICROLISP_API const char *microlisp_version(void);

/** Human-readable name of a status code. Never NULL, even for unknown
 *  codes. Returns short tokens like "MICROLISP_OK",
 *  "MICROLISP_ERR_UNBOUND". For a fuller, error-specific message use
 *  ::microlisp_state_error. */
MICROLISP_API const char *microlisp_status_string(microlisp_status status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MICROLISP_MICROLISP_H */
