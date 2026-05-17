/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 */
/**
 * @file main.c
 * @brief `microlisp` CLI: REPL, single-expression eval, and script runner.
 *
 * Usage:
 *   microlisp                   Interactive REPL on stdin.
 *   microlisp FILE              Run FILE as a script (one or more top-level
 *                               forms); print the last form's result.
 *   microlisp -                 Run stdin as a script (non-interactive).
 *   microlisp -e EXPR           Evaluate EXPR and print its result.
 *   microlisp --help | -h
 *   microlisp --version | -V
 *
 * Exit codes (stable for shell scripting):
 *   0  success
 *   1  evaluation error (read syntax, type error, unbound variable, etc.)
 *   2  I/O error reading the input or writing to stdout, or an unknown
 *      option / unknown subcommand on the command line
 */
#include "microlisp/microlisp.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- I/O helpers ----------------------------------------------------------- */

static int grow_buf(uint8_t **buf, size_t *cap) {
    if (*cap > SIZE_MAX / 2U) {
        free(*buf);
        *buf = NULL;
        return 1;
    }
    size_t new_cap = *cap * 2U;
    uint8_t *grown = realloc(*buf, new_cap);
    if (grown == NULL) {
        free(*buf);
        *buf = NULL;
        return 1;
    }
    *buf = grown;
    *cap = new_cap;
    return 0;
}

static FILE *open_input(const char *path) {
    if (path == NULL || strcmp(path, "-") == 0) {
        return stdin;
    }
    return fopen(path, "rb");
}

/* Slurp @p in into a freshly-allocated buffer. Returns 0 on success, 1 on
 * I/O failure, 2 on allocation failure. Caller owns @p *out_bytes on
 * success and must free it. */
static int slurp(FILE *in, uint8_t **out_bytes, size_t *out_len) {
    size_t cap = 4096;
    size_t len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (buf == NULL) {
        return 2;
    }
    for (;;) {
        size_t room = cap - len;
        size_t got = fread(buf + len, 1, room, in);
        len += got;
        if (got < room) {
            if (ferror(in)) {
                free(buf);
                return 1;
            }
            break; /* EOF */
        }
        if (grow_buf(&buf, &cap) != 0) {
            return 2;
        }
    }
    *out_bytes = buf;
    *out_len = len;
    return 0;
}

/* -- Usage + version ------------------------------------------------------- */

static void print_usage(FILE *out) {
    fprintf(out, "Usage:\n"
                 "  microlisp                Interactive REPL on stdin\n"
                 "  microlisp FILE           Run FILE as a script\n"
                 "  microlisp -              Run stdin as a script\n"
                 "  microlisp -e EXPR        Evaluate EXPR and print its result\n"
                 "  microlisp --help | -h\n"
                 "  microlisp --version | -V\n");
}

static void print_version(FILE *out) {
    fprintf(out, "microlisp %s\n", microlisp_version());
}

/* -- Exit-code mapping ----------------------------------------------------- */

static int exit_code_for(microlisp_status st) {
    switch (st) {
    case MICROLISP_OK:
        return 0;
    case MICROLISP_ERR_IO:
        return 2;
    case MICROLISP_ERR_INVALID_ARG:
    case MICROLISP_ERR_NOMEM:
    case MICROLISP_ERR_READ_SYNTAX:
    case MICROLISP_ERR_READ_TRUNCATED:
    case MICROLISP_ERR_READ_DEPTH:
    case MICROLISP_ERR_UNBOUND:
    case MICROLISP_ERR_TYPE:
    case MICROLISP_ERR_ARITY:
    case MICROLISP_ERR_DIV_ZERO:
    case MICROLISP_ERR_OVERFLOW:
    case MICROLISP_ERR_USER:
    case MICROLISP_ERR_SYNTAX:
    case MICROLISP_ERR_EVAL_DEPTH:
        return 1;
    default:
        return 1;
    }
}

/* -- Subcommand handlers --------------------------------------------------- */

static int eval_source(microlisp_state *state, const char *source, size_t len, int print_result) {
    char *result = NULL;
    size_t result_len = 0;
    microlisp_status st = microlisp_eval(state, source, len, &result, &result_len);
    if (st != MICROLISP_OK) {
        fprintf(stderr, "microlisp: error (%s): %s\n", microlisp_status_string(st),
                microlisp_state_error(state));
        microlisp_buffer_free(state, result);
        return exit_code_for(st);
    }
    if (print_result && result != NULL && result_len > 0) {
        /* Explicit flush so a broken pipe surfaces here as exit 2
         * rather than silently at process teardown. */
        if (fwrite(result, 1, result_len, stdout) != result_len || fputc('\n', stdout) == EOF ||
            fflush(stdout) != 0) {
            microlisp_buffer_free(state, result);
            return 2;
        }
    }
    microlisp_buffer_free(state, result);
    return 0;
}

static int run_repl(microlisp_state *state) {
    /* Use prompts only when stdin is a terminal so piped scripts stay
     * tidy. */
    const char *prompt = "microlisp> ";
    microlisp_status st = microlisp_repl(state, stdin, stdout, prompt);
    return exit_code_for(st);
}

static int run_script_file(microlisp_state *state, const char *path) {
    FILE *in = open_input(path);
    if (in == NULL) {
        fprintf(stderr, "microlisp: cannot open `%s`\n", path);
        return 2;
    }
    uint8_t *bytes = NULL;
    size_t len = 0;
    int rc = slurp(in, &bytes, &len);
    if (in != stdin) {
        fclose(in);
    }
    if (rc != 0) {
        fprintf(stderr, "microlisp: read error\n");
        return 2;
    }
    int result = eval_source(state, (const char *)bytes, len, /*print_result=*/0);
    free(bytes);
    return result;
}

/* -- Main ------------------------------------------------------------------ */

int main(int argc, char **argv) {
#ifdef SIGPIPE
    /* If stdout is piped into `head` (or any reader that closes early)
     * the default SIGPIPE handler kills the process with exit 141. By
     * ignoring SIGPIPE we let the failed write return EINVAL and the
     * documented "I/O error -> exit 2" contract holds. */
    signal(SIGPIPE, SIG_IGN);
#endif

    /* Argument parsing: walk argv looking for known options and a
     * subcommand-or-file argument. */
    int i = 1;
    const char *eval_expr = NULL;
    const char *script_path = NULL;
    int run_interactive = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            print_version(stdout);
            return 0;
        }
        if (strcmp(a, "-e") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "microlisp: -e requires an argument\n");
                print_usage(stderr);
                return 2;
            }
            eval_expr = argv[++i];
            run_interactive = 0;
            continue;
        }
        if (a[0] == '-' && a[1] != '\0' && strcmp(a, "-") != 0) {
            fprintf(stderr, "microlisp: unknown option `%s`\n", a);
            print_usage(stderr);
            return 2;
        }
        /* Positional: treat as script file (or `-` for stdin). */
        script_path = a;
        run_interactive = 0;
        break;
    }
    if (i < argc - 1) {
        /* Trailing positional args after the script path are reserved
         * for a future (script-args) feature; for now, reject. */
        fprintf(stderr, "microlisp: extra arguments after script path\n");
        return 2;
    }

    microlisp_state *state = NULL;
    microlisp_status st = microlisp_state_create(NULL, &state);
    if (st != MICROLISP_OK) {
        fprintf(stderr, "microlisp: cannot create interpreter state (%s)\n",
                microlisp_status_string(st));
        return 1;
    }

    int rc;
    if (eval_expr != NULL) {
        rc = eval_source(state, eval_expr, strlen(eval_expr), /*print_result=*/1);
    } else if (script_path != NULL) {
        rc = run_script_file(state, script_path);
    } else if (run_interactive) {
        rc = run_repl(state);
    } else {
        rc = 0;
    }

    microlisp_state_destroy(state);
    return rc;
}
