/*
 * SPDX-License-Identifier: MIT
 *
 * libFuzzer harness for the reader path only. Drives microlisp_eval
 * with a state that has the evaluator effectively unreachable -- we
 * feed source that should error before any user-defined form runs,
 * but the reader still has to walk every byte.
 *
 * In practice we just call microlisp_eval and accept any status; the
 * goal is to crash the reader on bytes it doesn't expect, not to
 * assert a particular outcome. ASan + UBSan provide the actual
 * oracle (out-of-bounds reads, NULL derefs, integer UB).
 */
#include "microlisp/microlisp.h"

#include <stddef.h>
#include <stdint.h>

/* NOLINTNEXTLINE(readability-identifier-naming) */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* NOLINTNEXTLINE(readability-identifier-naming) */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    microlisp_state *s = NULL;
    if (microlisp_state_create(NULL, &s) != MICROLISP_OK) {
        return 0;
    }
    char *result = NULL;
    size_t result_len = 0;
    (void)microlisp_eval(s, (const char *)data, size, &result, &result_len);
    microlisp_buffer_free(s, result);
    microlisp_state_destroy(s);
    return 0;
}
