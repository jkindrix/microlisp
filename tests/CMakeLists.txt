# Unit harness.
add_executable(test_microlisp test_microlisp.c)
target_link_libraries(test_microlisp PRIVATE microlisp::microlisp)
microlisp_apply_warnings(test_microlisp)
microlisp_apply_sanitizers(test_microlisp)

add_test(NAME unit_tests COMMAND test_microlisp)
set_tests_properties(unit_tests PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL:"
    TIMEOUT 60
)

# -- CLI smoke tests -------------------------------------------------------- #

add_test(NAME cli_eval_basic
    COMMAND $<TARGET_FILE:microlisp_cli> -e "(+ 1 2 3)"
)
set_tests_properties(cli_eval_basic PROPERTIES
    PASS_REGULAR_EXPRESSION "^6\r?\n$"
    TIMEOUT 10
)

add_test(NAME cli_eval_factorial
    COMMAND $<TARGET_FILE:microlisp_cli> -e
        "(define (f n) (if (= n 0) 1 (* n (f (- n 1))))) (f 10)"
)
set_tests_properties(cli_eval_factorial PROPERTIES
    PASS_REGULAR_EXPRESSION "^3628800\r?\n$"
    TIMEOUT 10
)

add_test(NAME cli_eval_tco
    # 200k tail calls -- a non-TCO interpreter would blow the C stack.
    COMMAND $<TARGET_FILE:microlisp_cli> -e
        "(define (loop n) (if (= n 0) (quote done) (loop (- n 1)))) (loop 200000)"
)
set_tests_properties(cli_eval_tco PROPERTIES
    PASS_REGULAR_EXPRESSION "^done\r?\n$"
    TIMEOUT 15
)

add_test(NAME cli_script_via_stdin
    COMMAND bash -c
        "printf '(define x 10) (define y 32) (+ x y)\n' | $<TARGET_FILE:microlisp_cli> -"
)
set_tests_properties(cli_script_via_stdin PROPERTIES
    TIMEOUT 10
)

add_test(NAME cli_version
    COMMAND microlisp_cli --version
)
set_tests_properties(cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^microlisp [0-9]+\\.[0-9]+\\.[0-9]+\r?\n$"
    TIMEOUT 10
)

add_test(NAME cli_help
    COMMAND microlisp_cli --help
)
set_tests_properties(cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage:"
    TIMEOUT 10
)

# Error path: unbound variable must exit 1 AND mention the variable.
# WILL_FAIL + PASS_REGULAR_EXPRESSION don't compose cleanly, so wrap
# both assertions in a single shell command.
if(CMAKE_HOST_UNIX)
    add_test(NAME cli_unbound_variable
        COMMAND bash -c
          "out=$($<TARGET_FILE:microlisp_cli> -e 'no-such-thing' 2>&1); \
           rc=$?; echo \"$out\"; \
           test $rc -eq 1 && echo \"$out\" | grep -q 'unbound'"
    )
    set_tests_properties(cli_unbound_variable PROPERTIES TIMEOUT 10)

    add_test(NAME cli_unknown_option
        COMMAND bash -c
          "out=$($<TARGET_FILE:microlisp_cli> --bogus 2>&1); rc=$?; \
           echo \"$out\"; test $rc -eq 2 && echo \"$out\" | grep -q 'unknown option'"
    )
    set_tests_properties(cli_unknown_option PROPERTIES TIMEOUT 10)

    # SIGPIPE handling -- when stdout is closed early the CLI should exit 2
    # (its documented I/O code), not 141. We pipe a `display` script into
    # `head -n 0` to slam the read side closed immediately.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The CLI writes in a loop so the broken pipe is guaranteed to
        # be observed: a one-shot `(display "hello")` can finish writing
        # into the FILE buffer before head closes the read end, which
        # under fast pipes makes the test racy. The loop forces enough
        # write attempts that one will fail.
        add_test(NAME cli_broken_pipe_exit_code
            COMMAND bash -c
              "$<TARGET_FILE:microlisp_cli> -e \
                 '(define (loop) (display \"x\") (loop)) (loop)' | head -n 0; \
               test \${PIPESTATUS[0]} -eq 2"
        )
        set_tests_properties(cli_broken_pipe_exit_code PROPERTIES TIMEOUT 10)
    endif()
endif()

# -- Symbol export check (shared builds only) ------------------------------- #
if(BUILD_SHARED_LIBS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME symbol_export_check
        COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/check_exports.sh"
            $<TARGET_FILE:microlisp>
    )
    set_tests_properties(symbol_export_check PROPERTIES TIMEOUT 30)

    add_test(NAME symbol_export_check_negative
        COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/check_exports_negative.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/check_exports.sh"
    )
    set_tests_properties(symbol_export_check_negative PROPERTIES TIMEOUT 30)
endif()

# -- Doxygen-coverage policy test (POSIX only) ----------------------------- #
if(CMAKE_HOST_UNIX)
    add_test(NAME doxygen_coverage_check
        COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/check_doxygen_coverage.sh"
            "${CMAKE_SOURCE_DIR}/include/microlisp/microlisp.h"
    )
    set_tests_properties(doxygen_coverage_check PROPERTIES TIMEOUT 10)

    add_test(NAME doxygen_coverage_check_negative
        COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/check_doxygen_coverage_negative.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/check_doxygen_coverage.sh"
    )
    set_tests_properties(doxygen_coverage_check_negative PROPERTIES TIMEOUT 10)
endif()

# -- Bake sanitizer runtime options into every test (see §8 of the
# -- project-principles reference for the why). Put this at the end of
# -- the directory so DIRECTORY PROPERTY TESTS enumerates all tests
# -- defined above.
get_property(_microlisp_all_tests DIRECTORY PROPERTY TESTS)
if(MICROLISP_ENABLE_ASAN OR MICROLISP_ENABLE_UBSAN)
    foreach(_t IN LISTS _microlisp_all_tests)
        set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT
            "ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:halt_on_error=1"
            "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
    endforeach()
endif()
if(MICROLISP_ENABLE_TSAN)
    foreach(_t IN LISTS _microlisp_all_tests)
        set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT
            "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1")
    endforeach()
endif()
if(MICROLISP_ENABLE_MSAN)
    foreach(_t IN LISTS _microlisp_all_tests)
        set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT
            "MSAN_OPTIONS=halt_on_error=1:print_stats=0")
    endforeach()
endif()
