cmake_minimum_required(VERSION 3.20)

# --------------------------------------------------------------------------- #
# Project metadata
# --------------------------------------------------------------------------- #
project(microlisp
    VERSION 0.1.1
    DESCRIPTION "A small Scheme-subset interpreter with mark-and-sweep GC, in modern C."
    LANGUAGES C
)

# --------------------------------------------------------------------------- #
# Standards & global settings
# --------------------------------------------------------------------------- #
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

set(MICROLISP_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MICROLISP_IS_TOP_LEVEL ON)
endif()

# --------------------------------------------------------------------------- #
# Options
# --------------------------------------------------------------------------- #
option(MICROLISP_BUILD_TESTS      "Build unit tests"            ${MICROLISP_IS_TOP_LEVEL})
option(MICROLISP_BUILD_EXAMPLES   "Build example programs"      ${MICROLISP_IS_TOP_LEVEL})
option(MICROLISP_BUILD_CLI        "Build the microlisp REPL"    ${MICROLISP_IS_TOP_LEVEL})
option(MICROLISP_WARNINGS_AS_ERRORS "Treat warnings as errors"  ${MICROLISP_IS_TOP_LEVEL})
option(MICROLISP_ENABLE_ASAN      "Enable AddressSanitizer"     OFF)
option(MICROLISP_ENABLE_UBSAN     "Enable UndefinedBehaviorSanitizer" OFF)
option(MICROLISP_ENABLE_MSAN      "Enable MemorySanitizer (clang only)" OFF)
option(MICROLISP_ENABLE_TSAN      "Enable ThreadSanitizer"      OFF)
option(MICROLISP_ENABLE_COVERAGE  "Enable code-coverage instrumentation" OFF)
option(MICROLISP_BUILD_FUZZERS    "Build libFuzzer-based fuzz targets (Clang only)" OFF)
option(BUILD_SHARED_LIBS          "Build libmicrolisp as a shared library" OFF)

# --------------------------------------------------------------------------- #
# Helper modules
# --------------------------------------------------------------------------- #
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

# --------------------------------------------------------------------------- #
# Generated version header
# --------------------------------------------------------------------------- #
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/microlisp/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/microlisp/version.h"
    @ONLY
)

# --------------------------------------------------------------------------- #
# Library target
# --------------------------------------------------------------------------- #
# Source list, factored out so the fuzz subdirectory can build a separately-
# instrumented copy of the library (see tests/fuzz/CMakeLists.txt).
set(MICROLISP_LIB_SOURCES
    src/gc.c
    src/value.c
    src/symtab.c
    src/env.c
    src/reader.c
    src/printer.c
    src/eval.c
    src/primitives.c
    src/state.c
    src/status.c
    src/version.c
)

add_library(microlisp ${MICROLISP_LIB_SOURCES})
add_library(microlisp::microlisp ALIAS microlisp)

target_include_directories(microlisp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>
        $<INSTALL_INTERFACE:include>
)
target_include_directories(microlisp PRIVATE src)

set_target_properties(microlisp PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME microlisp
    POSITION_INDEPENDENT_CODE ON
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(microlisp PRIVATE MICROLISP_BUILD_SHARED)
    target_compile_definitions(microlisp INTERFACE MICROLISP_USE_SHARED)
endif()

microlisp_apply_warnings(microlisp)
microlisp_apply_sanitizers(microlisp)

# --------------------------------------------------------------------------- #
# CLI executable
# --------------------------------------------------------------------------- #
if(MICROLISP_BUILD_CLI)
    add_executable(microlisp_cli src/main.c)
    set_target_properties(microlisp_cli PROPERTIES OUTPUT_NAME microlisp)
    target_link_libraries(microlisp_cli PRIVATE microlisp::microlisp)
    microlisp_apply_warnings(microlisp_cli)
    microlisp_apply_sanitizers(microlisp_cli)
endif()

# --------------------------------------------------------------------------- #
# Examples
# --------------------------------------------------------------------------- #
if(MICROLISP_BUILD_EXAMPLES)
    add_executable(microlisp_embed_example examples/embed_example.c)
    target_link_libraries(microlisp_embed_example PRIVATE microlisp::microlisp)
    microlisp_apply_warnings(microlisp_embed_example)
    microlisp_apply_sanitizers(microlisp_embed_example)
endif()

# --------------------------------------------------------------------------- #
# Tests
# --------------------------------------------------------------------------- #
if(MICROLISP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MICROLISP_BUILD_FUZZERS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        add_subdirectory(tests/fuzz)
    else()
        message(WARNING
            "MICROLISP_BUILD_FUZZERS=ON ignored: libFuzzer requires Clang "
            "(got ${CMAKE_C_COMPILER_ID}). Re-run with CC=clang to enable.")
        set(MICROLISP_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    endif()
endif()

# --------------------------------------------------------------------------- #
# Install / packaging
# --------------------------------------------------------------------------- #
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(_install_targets microlisp)
if(MICROLISP_BUILD_CLI)
    list(APPEND _install_targets microlisp_cli)
endif()

install(TARGETS ${_install_targets}
    EXPORT microlispTargets
    RUNTIME       DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY       DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE       DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/microlisp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/microlisp/version.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/microlisp
)

install(EXPORT microlispTargets
    FILE microlispTargets.cmake
    NAMESPACE microlisp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/microlisp
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/microlispConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/microlispConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/microlispConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/microlisp
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/microlispConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/microlispConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/microlisp
)

# Relocatable pkg-config -- the prefix is derived from this file's installed
# location at runtime, not baked in at configure time.
file(RELATIVE_PATH MICROLISP_PC_RELATIVE_PREFIX
    "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig"
    "${CMAKE_INSTALL_PREFIX}"
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/microlisp.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/microlisp.pc"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/microlisp.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# --------------------------------------------------------------------------- #
# Summary
# --------------------------------------------------------------------------- #
message(STATUS "microlisp ${PROJECT_VERSION}")
message(STATUS "  C standard:        C${CMAKE_C_STANDARD}")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  Shared library:    ${BUILD_SHARED_LIBS}")
message(STATUS "  CLI:               ${MICROLISP_BUILD_CLI}")
message(STATUS "  Tests:             ${MICROLISP_BUILD_TESTS}")
message(STATUS "  Examples:          ${MICROLISP_BUILD_EXAMPLES}")
message(STATUS "  ASan/UBSan/MSan/TSan: ${MICROLISP_ENABLE_ASAN}/${MICROLISP_ENABLE_UBSAN}/${MICROLISP_ENABLE_MSAN}/${MICROLISP_ENABLE_TSAN}")
message(STATUS "  Coverage:          ${MICROLISP_ENABLE_COVERAGE}")
