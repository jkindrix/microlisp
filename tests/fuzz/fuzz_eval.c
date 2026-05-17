/*
 * SPDX-License-Identifier: MIT
 *
 * libFuzzer harness for read + eval. Same shape as fuzz_read.c -- we
 * accept any status and rely on ASan + UBSan to detect actual bugs.
 *
 * Because the language is Turing-complete, a fuzz input can produce
 * an unbounded loop. libFuzzer's per-input timeout (the harness is
 * invoked with `-timeout=4` by default in CI) terminates such inputs;
 * the fuzzer treats a timeout as a finding worth saving but won't
 * stall the run.
 *
 * We construct a fresh state per input so accumulated GC pressure
 * from one input doesn't bleed into the next.
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
