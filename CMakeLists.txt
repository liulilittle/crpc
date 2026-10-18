cmake_minimum_required(VERSION 3.16)
project(crpc VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(CRPC_BUILD_TESTS "Build the test suite" ON)
option(CRPC_BUILD_BENCH "Build microbenchmarks" OFF)
option(CRPC_BUILD_EXAMPLES "Build the examples" OFF)
option(CRPC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(CRPC_ENABLE_LTO "Enable link-time optimisation in Release builds" ON)

set(CRPC_SANITIZER "" CACHE STRING
    "Sanitizer to enable: empty, address, undefined, or thread")

# Dependency locations. Both are consumed read-only through their public
# headers and static libraries. Set these to the checkout roots.
set(CRPC_RTTI_DIR "" CACHE PATH "Root of the RTTI checkout")
set(CRPC_COROCC_DIR "" CACHE PATH "Root of the corocc checkout")

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    # /utf-8 keeps source encoding stable; /permissive- enforces standard
    # conformance; /Zc:preprocessor is required by the RTTI macro machinery.
    add_compile_options(/utf-8 /permissive- /Zc:preprocessor /Zc:__cplusplus)
    if(CRPC_WARNINGS_AS_ERRORS)
        add_compile_options(/W4 /WX)
    else()
        add_compile_options(/W4)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(CRPC_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

if(CRPC_SANITIZER)
    if(MSVC)
        add_compile_options(/fsanitize=${CRPC_SANITIZER})
    else()
        add_compile_options(-fsanitize=${CRPC_SANITIZER} -fno-omit-frame-pointer)
        add_link_options(-fsanitize=${CRPC_SANITIZER})
    endif()
endif()

if(CRPC_ENABLE_LTO AND NOT CRPC_SANITIZER)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT CRPC_IPO_SUPPORTED OUTPUT CRPC_IPO_ERROR)
    if(CRPC_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

if(CRPC_RTTI_DIR)
    if(NOT EXISTS "${CRPC_RTTI_DIR}/include/rtti/reflect.hpp")
        message(FATAL_ERROR "CRPC_RTTI_DIR does not look like an RTTI checkout: ${CRPC_RTTI_DIR}")
    endif()
    add_subdirectory("${CRPC_RTTI_DIR}" "${CMAKE_BINARY_DIR}/_deps/rtti" EXCLUDE_FROM_ALL)
    set(CRPC_HAVE_RTTI ON)
else()
    message(STATUS "CRPC_RTTI_DIR not set: RTTI-dependent components are disabled")
    set(CRPC_HAVE_RTTI OFF)
endif()

if(CRPC_COROCC_DIR)
    if(NOT EXISTS "${CRPC_COROCC_DIR}/include/corocc/coro.h")
        message(FATAL_ERROR "CRPC_COROCC_DIR does not look like a corocc checkout: ${CRPC_COROCC_DIR}")
    endif()
    set(COROCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CRPC_COROCC_DIR}" "${CMAKE_BINARY_DIR}/_deps/corocc" EXCLUDE_FROM_ALL)
    set(CRPC_HAVE_COROCC ON)
else()
    message(STATUS "CRPC_COROCC_DIR not set: coroutine-backed components are disabled")
    set(CRPC_HAVE_COROCC OFF)
endif()

# ---------------------------------------------------------------------------
# Core library (no third-party dependency)
# ---------------------------------------------------------------------------

add_library(crpc_core STATIC
    src/error.cpp
)

target_include_directories(crpc_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(crpc_core PUBLIC cxx_std_17)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(CRPC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CRPC_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(CRPC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
