/*
 * SPDX-License-Identifier: MIT
 *
 * Symbol-visibility macros and small attribute helpers. Public declarations
 * carry MICROLISP_API so shared-library builds can hide everything else with
 * -fvisibility=hidden. The attribute helpers (MICROLISP_NODISCARD,
 * MICROLISP_NORETURN, MICROLISP_PRINTF) are also exposed here because callers
 * benefit from the compile-time enforcement they provide.
 */
#ifndef MICROLISP_EXPORT_H
#define MICROLISP_EXPORT_H

/* -- MICROLISP_API: public-symbol visibility -------------------------------- */
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(MICROLISP_BUILD_SHARED)
#define MICROLISP_API __declspec(dllexport)
#elif defined(MICROLISP_USE_SHARED)
#define MICROLISP_API __declspec(dllimport)
#else
#define MICROLISP_API
#endif
#else
#if defined(MICROLISP_BUILD_SHARED) && (defined(__GNUC__) || defined(__clang__))
#define MICROLISP_API __attribute__((visibility("default")))
#else
#define MICROLISP_API
#endif
#endif

/* -- MICROLISP_NODISCARD: warn if the return value is ignored --------------- */
#if defined(__GNUC__) || defined(__clang__)
#define MICROLISP_NODISCARD __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#define MICROLISP_NODISCARD _Check_return_
#else
#define MICROLISP_NODISCARD
#endif

/* -- MICROLISP_NORETURN: function doesn't return ---------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#define MICROLISP_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define MICROLISP_NORETURN __declspec(noreturn)
#else
#define MICROLISP_NORETURN
#endif

/* -- MICROLISP_PRINTF: type-check printf-style args ------------------------- */
#if defined(__GNUC__) || defined(__clang__)
/* On MinGW the `printf` archetype maps to MSVCRT's printf, which doesn't
 * support %zu / %lld and friends. We use gnu_printf there so the format
 * checker matches the ANSI-stdio path the library actually links against. */
#if defined(__MINGW32__) || defined(__MINGW64__)
#define MICROLISP_PRINTF(fmt_idx, vararg_idx)                                                      \
    __attribute__((format(gnu_printf, fmt_idx, vararg_idx)))
#else
#define MICROLISP_PRINTF(fmt_idx, vararg_idx) __attribute__((format(printf, fmt_idx, vararg_idx)))
#endif
#else
#define MICROLISP_PRINTF(fmt_idx, vararg_idx)
#endif

#endif /* MICROLISP_EXPORT_H */
