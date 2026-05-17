/*
 * SPDX-License-Identifier: MIT
 *
 * Downstream consumer used by CI to verify find_package(microlisp)
 * resolves cleanly against an installed package.
 */
#include <microlisp/microlisp.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    microlisp_state *s = NULL;
    microlisp_status st = microlisp_state_create(NULL, &s);
    if (st != MICROLISP_OK) {
        fprintf(stderr, "consumer: state_create failed: %s\n", microlisp_status_string(st));
        return 1;
    }
    const char *src = "(+ 1 2 3)";
    char *result = NULL;
    size_t result_len = 0;
    st = microlisp_eval(s, src, strlen(src), &result, &result_len);
    if (st != MICROLISP_OK) {
        fprintf(stderr, "consumer: eval failed: %s\n", microlisp_status_string(st));
        microlisp_state_destroy(s);
        return 1;
    }
    printf("linked against libmicrolisp %s; result=%s\n", microlisp_version(),
           result != NULL ? result : "(unspecified)");
    microlisp_buffer_free(s, result);
    microlisp_state_destroy(s);
    return 0;
}
