/*
 * SPDX-License-Identifier: MIT
 *
 * libFuzzer harness for the reader path only.
 *
 * Unlike fuzz_eval (which drives the full Turing-complete pipeline
 * via microlisp_eval and so can legitimately OOM or time out on a
 * crafted infinite loop), this harness calls ml_read directly. The
 * reader's runtime is bounded by the input size, so any timeout or
 * OOM here is a real bug.
 *
 * Each read'd form is pushed onto the protect stack so that the
 * next read's allocations don't reclaim it -- this exercises the
 * full GC-during-reader path that one-shot inputs miss.
 *
 * ASan + UBSan provide the actual oracle (out-of-bounds reads, NULL
 * derefs, integer UB, leak detection at state destroy).
 */
#include "microlisp/microlisp.h"

#include "microlisp_internal.h"

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
    ml_reader r = {
        .input = data,
        .len = size,
        .pos = 0,
        .line = 1,
        .column = 1,
        .depth = 0,
    };
    /* Read until EOF or a parse error; protect every form so the GC
     * has to walk the accumulated tree on each subsequent allocation. */
    for (;;) {
        mvalue form = MV_UNDEF;
        microlisp_status st = ml_read(s, &r, &form);
        if (st != MICROLISP_OK || form == MV_EOF) {
            break;
        }
        if (ml_gc_protect(s, form) != MICROLISP_OK) {
            break;
        }
    }
    microlisp_state_destroy(s);
    return 0;
}
