# Fuzz target build. Clang-only; the gate lives in the top-level
# CMakeLists.txt which auto-disables MICROLISP_BUILD_FUZZERS on non-Clang
# toolchains. Reaching this file means the check has already passed.
#
# As in bencode, we build a *separate* copy of the library here with
# -fsanitize=fuzzer-no-link,address,undefined on every TU so libFuzzer's
# coverage feedback + the sanitizers reach the interpreter, not just
# the tiny harness wrappers.

set(MICROLISP_FUZZ_SANITIZE -fsanitize=fuzzer-no-link,address,undefined)

set(_fuzz_lib_sources)
foreach(_src IN LISTS MICROLISP_LIB_SOURCES)
    list(APPEND _fuzz_lib_sources "${CMAKE_SOURCE_DIR}/${_src}")
endforeach()

add_library(microlisp_fuzz STATIC ${_fuzz_lib_sources})
target_include_directories(microlisp_fuzz
    PUBLIC
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated/include"
        # PUBLIC: fuzz_read.c drives the reader directly via the
        # internal API, so it needs to see microlisp_internal.h.
        "${CMAKE_SOURCE_DIR}/src"
)
target_compile_features(microlisp_fuzz PRIVATE c_std_17)
target_compile_options(microlisp_fuzz PRIVATE
    ${MICROLISP_FUZZ_SANITIZE}
    -fno-omit-frame-pointer
    -fvisibility=hidden
)
target_link_options(microlisp_fuzz INTERFACE ${MICROLISP_FUZZ_SANITIZE})
microlisp_apply_warnings(microlisp_fuzz)

function(microlisp_add_fuzz_target name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE microlisp_fuzz)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${name}    PRIVATE -fsanitize=fuzzer,address,undefined)
    microlisp_apply_warnings(${name})
endfunction()

microlisp_add_fuzz_target(fuzz_read)
microlisp_add_fuzz_target(fuzz_eval)
