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
option(CRPC_BUILD_SHARED "Build crpc as a shared library" OFF)
option(CRPC_WITH_OPENSSL "Enable the OpenSSL TLS backend (HTTPS)" OFF)

set(CRPC_SANITIZER "" CACHE STRING
    "Sanitizer to enable: empty, address, undefined, or thread")

# Dependency locations. Both are consumed read-only through their public
# headers and static libraries. Set these to the checkout roots, either as cache
# variables or through the environment (the one-click scripts forward the
# environment without needing extra arguments).
set(CRPC_RTTI_DIR "" CACHE PATH "Root of the RTTI checkout")
set(CRPC_COROCC_DIR "" CACHE PATH "Root of the corocc checkout")

if(NOT CRPC_RTTI_DIR AND DEFINED ENV{CRPC_RTTI_DIR})
    set(CRPC_RTTI_DIR "$ENV{CRPC_RTTI_DIR}" CACHE PATH "Root of the RTTI checkout" FORCE)
endif()
if(NOT CRPC_COROCC_DIR AND DEFINED ENV{CRPC_COROCC_DIR})
    set(CRPC_COROCC_DIR "$ENV{CRPC_COROCC_DIR}" CACHE PATH "Root of the corocc checkout" FORCE)
endif()

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
    # The conformance and warning flags below are C/C++ compiler options, not
    # assembler options. They are attached at directory scope so that every
    # in-tree target inherits them, but that scope also reaches the vendored
    # corocc MASM backend (ml64/ml under the Visual Studio generator). Scoping
    # each option with a COMPILE_LANGUAGE generator expression keeps them on
    # C and C++ sources only: ml64 otherwise rejects /W4 /WX /utf-8 as
    # "invalid command-line option" (A4018/A4008), which MSB3721 turns into a
    # hard build failure and the missing x86_64_ms.obj then breaks the link.
    #
    # /utf-8 keeps source encoding stable; /permissive- enforces standard
    # conformance; /Zc:preprocessor is required by the RTTI macro machinery.
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:/permissive->"
        "$<$<COMPILE_LANGUAGE:C,CXX>:/Zc:preprocessor>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:/Zc:__cplusplus>"
    )
    # NOMINMAX and WIN32_LEAN_AND_MEAN are defined for every target: a public
    # header that ends up including <windows.h> would otherwise turn min/max
    # into macros and break std::min/std::max in consumer code.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    if(CRPC_WARNINGS_AS_ERRORS)
        add_compile_options(
            "$<$<COMPILE_LANGUAGE:C,CXX>:/W4>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:/WX>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:/we4062>"
        )
    else()
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/W4>")
    endif()
else()
    # GCC/Clang warning policy. It is kept in a variable and attached to the
    # crpc target below (target_compile_options) rather than to the directory
    # (add_compile_options). Directory-scope options are inherited by every
    # add_subdirectory(), including the vendored corocc checkout further down,
    # whose own -Werror then turns crpc's -Wpedantic into hard errors in
    # third-party code crpc does not own. PUBLIC scope still propagates the
    # policy to every in-tree consumer (tests, bench, examples) through their
    # link to crpc, so crpc's own code stays fully checked.
    set(CRPC_WARNING_OPTIONS -Wall -Wextra -Wpedantic)
    if(CRPC_WARNINGS_AS_ERRORS)
        list(APPEND CRPC_WARNING_OPTIONS -Werror)
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

# Without the RTTI checkout the suite still builds and runs: only the targets
# that register reflected types are dropped (see tests/CMakeLists.txt). A
# FATAL_ERROR here would reject a configuration that is known to work, so the
# gate is per target and this block only reports the consequence.
if(CRPC_BUILD_TESTS AND NOT CRPC_HAVE_RTTI)
    message(STATUS
        "CRPC: tests that use the RTTI reflection macros are skipped; "
        "set CRPC_RTTI_DIR (CMake variable or environment) to include them")
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
#
# The library builds either as a static archive or as a shared library. Both
# forms are first-class: a static build is the default because it has no
# runtime deployment requirements, and a shared build is what a plugin host or
# a multi-language deployment needs. The ABI rules in docs/abi.md apply to the
# shared form.
#
# CRPC_BUILD_SHARED=ON  -> shared library, CRPC_SHARED/CRPC_BUILDING defined
# CRPC_BUILD_SHARED=OFF -> static archive (default)

# CRPC_BUILD_SHARED is declared in the options block at the top.

set(CRPC_LIBRARY_TYPE STATIC)
if(CRPC_BUILD_SHARED)
    set(CRPC_LIBRARY_TYPE SHARED)
endif()

add_library(crpc ${CRPC_LIBRARY_TYPE})

# The warning policy is target-scoped, not directory-scoped, so it never
# reaches the vendored corocc checkout while still covering every target that
# links crpc (the library itself, tests, bench, examples). MSVC keeps its
# directory-level policy above: its options are language-guarded to C/CXX and
# already exclude the MASM backend.
if(NOT MSVC)
    target_compile_options(crpc PUBLIC ${CRPC_WARNING_OPTIONS})
endif()

# Sources are discovered by pattern so that independent subsystems can be added
# without editing this file. CONFIGURE_DEPENDS re-globs on reconfigure.
file(GLOB_RECURSE CRPC_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(crpc PRIVATE ${CRPC_CORE_SOURCES})

# The alias keeps test and consumer targets independent of the library type.
add_library(crpc::crpc ALIAS crpc)
add_library(crpc_core ALIAS crpc)

target_include_directories(crpc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(crpc PUBLIC cxx_std_17)

if(CRPC_BUILD_SHARED)
    target_compile_definitions(crpc PUBLIC CRPC_SHARED)
    target_compile_definitions(crpc PRIVATE CRPC_BUILDING)
    set_target_properties(crpc PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    if(MSVC)
        # C4251 warns that a class with a private STL member is exported. The
        # members are private and never appear in a signature; what they affect
        # is the object's size and layout, which docs/abi.md section 7 already
        # constrains to "same toolchain family and debug setting". Suppressing
        # the warning here records that decision rather than hiding an unknown
        # risk: a consumer built with a different STL is unsupported either way.
        target_compile_options(crpc PUBLIC /wd4251)
    endif()
    if(NOT MSVC)
        # Every exported symbol is marked CRPC_API; anything else stays hidden.
        target_link_options(crpc PRIVATE -Wl,--no-undefined)
    endif()
endif()

# ---------------------------------------------------------------------------
# Executable placement
# ---------------------------------------------------------------------------
#
# In a shared build every consumer executable imports crpc.dll, which is
# written to the build root. Windows resolves an import from the executable's
# own directory (then the system directories, then the current directory); it
# never searches the *parent* of the executable's directory. An executable left
# in its per-directory default location (tests/, bench/) would therefore die
# before main() with STATUS_DLL_NOT_FOUND (0xC0000135), which a test runner
# reports as a bare exit code with no output -- indistinguishable from a hang.
#
# The fix is placement, not copying: every consumer executable sets
# RUNTIME_OUTPUT_DIRECTORY to the build root (see tests/CMakeLists.txt and
# bench/CMakeLists.txt), so it is written next to crpc.dll and the loader finds
# the import with no PATH entry and no working-directory property. A POST_BUILD
# copy of the DLL would only run when the executable itself relinks, so a
# library-only rebuild would leave a stale crpc.dll beside the executable and a
# test run could pass against the old binary.
#
# Static builds use the same property: the executables sit next to crpc.lib and
# the layout is identical in both modes.

# Platform libraries the transport and timing layers need.
if(WIN32)
    target_link_libraries(crpc PUBLIC ws2_32 mswsock)
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(crpc PUBLIC Threads::Threads)
endif()

if(CRPC_HAVE_COROCC)
    target_link_libraries(crpc PUBLIC corocc)
    target_compile_definitions(crpc PUBLIC CRPC_HAVE_COROCC=1)
endif()

if(CRPC_HAVE_RTTI)
    target_link_libraries(crpc PUBLIC rtti)
    target_compile_definitions(crpc PUBLIC CRPC_HAVE_RTTI=1)
endif()

# --- Optional OpenSSL TLS backend -------------------------------------------
#
# Off by default: the library is fully functional without it (plain TCP and
# HTTP/1.1 + HTTP/2 over cleartext). Turning it on adds the HTTPS entry point.
# The public headers never include an OpenSSL header, so consumers that do not
# enable it are unaffected.

if(CRPC_WITH_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(crpc PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(crpc PUBLIC CRPC_HAVE_OPENSSL=1)
    message(STATUS "CRPC: OpenSSL ${OPENSSL_VERSION} enabled (HTTPS)")
else()
    message(STATUS "CRPC: OpenSSL disabled (no HTTPS)")
endif()


# The codec layer consumes RTTI's public reflection headers and (for JSON and
# the text/schema adapters) its compiled static library. Linking it PUBLIC also
# propagates the RTTI include directory to every crpc consumer. This is already
# covered by the CRPC_HAVE_RTTI block above; the duplicate was removed.

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(CRPC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)

    # RTTI's own CMakeLists calls enable_testing() and registers its test
    # binaries unconditionally. We add it with EXCLUDE_FROM_ALL (we only need
    # the library), so those entries exist in CTest but are never built, which
    # makes `ctest` report three "Not Run" failures. They belong to the
    # dependency's own CI, not to ours: exclude them by name rather than
    # patching the dependency.
    set(CRPC_EXTERNAL_TESTS
        rtti_tests
        rtti_proto_extension_tests
        rtti_flat_schema_tests
        rtti_bench)
    file(WRITE "${CMAKE_BINARY_DIR}/CTestCustom.cmake"
         "set(CTEST_CUSTOM_TESTS_IGNORE \${CTEST_CUSTOM_TESTS_IGNORE} ${CRPC_EXTERNAL_TESTS})\n")
endif()

if(CRPC_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(CRPC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

