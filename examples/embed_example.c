/*
 * SPDX-License-Identifier: MIT
 *
 * Minimal embedding example.
 *
 * Demonstrates creating a microlisp state, evaluating a short program
 * that mixes definitions and an expression, and printing the result.
 */
#include <microlisp/microlisp.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    microlisp_state *s = NULL;
    microlisp_status st = microlisp_state_create(NULL, &s);
    if (st != MICROLISP_OK) {
        fprintf(stderr, "state_create failed: %s\n", microlisp_status_string(st));
        return 1;
    }
    const char *program = "(define (factorial n)\n"
                          "  (if (<= n 1) 1 (* n (factorial (- n 1)))))\n"
                          "(factorial 10)\n";
    char *result = NULL;
    size_t result_len = 0;
    st = microlisp_eval(s, program, strlen(program), &result, &result_len);
    if (st != MICROLISP_OK) {
        fprintf(stderr, "eval failed: %s: %s\n", microlisp_status_string(st),
                microlisp_state_error(s));
        microlisp_state_destroy(s);
        return 1;
    }
    printf("factorial(10) = %s\n", result != NULL ? result : "(unspecified)");
    microlisp_buffer_free(s, result);
    microlisp_state_destroy(s);
    return 0;
}
