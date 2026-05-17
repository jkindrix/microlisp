# Centralized warning + hardening flags. Apply with microlisp_apply_warnings(<target>).
#
# Two responsibilities live here:
#   1. The aggressive baseline warning set.
#   2. Compile- and link-time hardening (FORTIFY, stack-protector,
#      file-prefix-map, PIE/RELRO/BIND_NOW). They share a module because every
#      target wants both and splitting would mean two near-identical apply
#      calls per target.

include(CheckCSourceCompiles)
include(CheckCCompilerFlag)

function(microlisp_apply_warnings target)
    if(MSVC)
        set(_warnings
            /W4
            /permissive-
            /w14242 /w14254 /w14263 /w14265 /w14287 /we4289
            /w14296 /w14311 /w14545 /w14546 /w14547 /w14549
            /w14555 /w14619 /w14640 /w14826 /w14905 /w14906
            /w14928
        )
        if(MICROLISP_WARNINGS_AS_ERRORS)
            list(APPEND _warnings /WX)
        endif()
    else()
        set(_warnings
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wcast-align
            -Wcast-qual
            -Wwrite-strings
            -Wstrict-prototypes
            -Wmissing-prototypes
            -Wmissing-declarations
            -Wredundant-decls
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
            -Wformat-security
            -Wundef
            -Wvla
            -Wpointer-arith
            -Wswitch-default
            -Wswitch-enum
            -Winit-self
        )
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            list(APPEND _warnings
                -Wduplicated-cond
                -Wduplicated-branches
                -Wlogical-op
                -Wjump-misses-init
                -Wtrampolines
            )
        endif()
        if(MICROLISP_WARNINGS_AS_ERRORS)
            list(APPEND _warnings -Werror)
        endif()
    endif()

    target_compile_options(${target} PRIVATE ${_warnings})

    # -- Hardening flags below. Skipped on MSVC entirely; MSVC's own
    # -- mitigations (/GS, /guard:cf, ASLR via /DYNAMICBASE) are on by default.
    if(MSVC)
        return()
    endif()

    # _FORTIFY_SOURCE=3 needs glibc >= 2.34 and GCC >= 12 / Clang >= 9, plus
    # -O1 or higher (hence the non-Debug gate). Detect once, cache the result.
    if(NOT DEFINED MICROLISP_FORTIFY_LEVEL_CACHED)
        set(CMAKE_REQUIRED_FLAGS "-O1 -Werror -D_FORTIFY_SOURCE=3")
        check_c_source_compiles("#include <string.h>
int main(void){char b[8]; strcpy(b,\"hi\"); return b[0];}" MICROLISP_HAVE_FORTIFY3)
        unset(CMAKE_REQUIRED_FLAGS)
        if(MICROLISP_HAVE_FORTIFY3)
            set(MICROLISP_FORTIFY_LEVEL_CACHED 3 CACHE INTERNAL "")
        else()
            set(MICROLISP_FORTIFY_LEVEL_CACHED 2 CACHE INTERNAL "")
        endif()
    endif()
    target_compile_definitions(${target} PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=${MICROLISP_FORTIFY_LEVEL_CACHED}>
    )
    target_compile_options(${target} PRIVATE -fstack-protector-strong)

    # -fstrict-flex-arrays=3 improves FORTIFY's coverage of flexible-array
    # members. Clang >= 16 / GCC >= 13.
    if(NOT DEFINED MICROLISP_HAVE_STRICT_FLEX_ARRAYS_CACHED)
        check_c_compiler_flag("-fstrict-flex-arrays=3" MICROLISP_HAVE_STRICT_FLEX_ARRAYS)
        set(MICROLISP_HAVE_STRICT_FLEX_ARRAYS_CACHED "${MICROLISP_HAVE_STRICT_FLEX_ARRAYS}"
            CACHE INTERNAL "")
    endif()
    if(MICROLISP_HAVE_STRICT_FLEX_ARRAYS_CACHED)
        target_compile_options(${target} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:-fstrict-flex-arrays=3>
        )
    endif()

    # -ffile-prefix-map: strip absolute build paths from __FILE__, assertion
    # strings, and DWARF info. GCC >= 8 / Clang >= 10.
    if(NOT DEFINED MICROLISP_HAVE_FILE_PREFIX_MAP_CACHED)
        check_c_compiler_flag("-ffile-prefix-map=/=/" MICROLISP_HAVE_FILE_PREFIX_MAP)
        set(MICROLISP_HAVE_FILE_PREFIX_MAP_CACHED "${MICROLISP_HAVE_FILE_PREFIX_MAP}"
            CACHE INTERNAL "")
    endif()
    if(MICROLISP_HAVE_FILE_PREFIX_MAP_CACHED)
        target_compile_options(${target} PRIVATE
            "$<$<NOT:$<CONFIG:Debug>>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.>"
            "$<$<NOT:$<CONFIG:Debug>>:-ffile-prefix-map=${CMAKE_BINARY_DIR}=./build>"
        )
    endif()

    # Link-time hardening for Linux executables. Static libs don't need PIE;
    # shared libs already get PIC via POSITION_INDEPENDENT_CODE.
    get_target_property(_target_type ${target} TYPE)
    if(_target_type STREQUAL "EXECUTABLE" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_property(TARGET ${target} PROPERTY POSITION_INDEPENDENT_CODE ON)
        target_link_options(${target} PRIVATE
            -pie
            -Wl,-z,relro
            -Wl,-z,now
            -Wl,-z,noexecstack
        )
    endif()
endfunction()
