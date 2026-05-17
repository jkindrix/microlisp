/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 microlisp contributors
 *
 * Status-code names. The token form ("MICROLISP_OK", "MICROLISP_ERR_UNBOUND")
 * is useful for tests and structured diagnostics; the human-readable
 * message for a *specific* failure lives in state.last_error and is
 * retrieved via microlisp_state_error.
 */

#include "microlisp/microlisp.h"

const char *microlisp_status_string(microlisp_status status) {
    switch (status) {
    case MICROLISP_OK:
        return "MICROLISP_OK";
    case MICROLISP_ERR_INVALID_ARG:
        return "MICROLISP_ERR_INVALID_ARG";
    case MICROLISP_ERR_NOMEM:
        return "MICROLISP_ERR_NOMEM";
    case MICROLISP_ERR_READ_SYNTAX:
        return "MICROLISP_ERR_READ_SYNTAX";
    case MICROLISP_ERR_READ_TRUNCATED:
        return "MICROLISP_ERR_READ_TRUNCATED";
    case MICROLISP_ERR_READ_DEPTH:
        return "MICROLISP_ERR_READ_DEPTH";
    case MICROLISP_ERR_UNBOUND:
        return "MICROLISP_ERR_UNBOUND";
    case MICROLISP_ERR_TYPE:
        return "MICROLISP_ERR_TYPE";
    case MICROLISP_ERR_ARITY:
        return "MICROLISP_ERR_ARITY";
    case MICROLISP_ERR_DIV_ZERO:
        return "MICROLISP_ERR_DIV_ZERO";
    case MICROLISP_ERR_OVERFLOW:
        return "MICROLISP_ERR_OVERFLOW";
    case MICROLISP_ERR_USER:
        return "MICROLISP_ERR_USER";
    case MICROLISP_ERR_IO:
        return "MICROLISP_ERR_IO";
    case MICROLISP_ERR_SYNTAX:
        return "MICROLISP_ERR_SYNTAX";
    case MICROLISP_ERR_EVAL_DEPTH:
        return "MICROLISP_ERR_EVAL_DEPTH";
    case MICROLISP_ERR_PRINT_DEPTH:
        return "MICROLISP_ERR_PRINT_DEPTH";
    case MICROLISP_ERR_EQUAL_DEPTH:
        return "MICROLISP_ERR_EQUAL_DEPTH";
    default:
        return "MICROLISP_ERR_UNKNOWN";
    }
}
