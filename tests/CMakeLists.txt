# CRPC test suite.
#
# Tests are discovered by pattern so that adding a test file never requires
# editing this file. That keeps parallel work on independent subsystems free of
# merge conflicts on the build script. CONFIGURE_DEPENDS makes CMake re-glob
# when a file is added or removed, so a stale configure is not a trap.

# The C ABI smoke test is a real C translation unit (not C++ compiled with a C
# driver), so the C language must be enabled for this directory.
enable_language(C)

# Every test executable is written to the build root, next to crpc.dll (shared
# builds) and crpc.lib (static builds). Windows resolves an import from the
# executable's own directory and then the current directory; it does not search
# the parent of the executable's directory, so a test staged into <build>/tests/
# would die before main() with STATUS_DLL_NOT_FOUND (0xC0000135).
#
# `RUNTIME_OUTPUT_DIRECTORY` is used instead of a POST_BUILD copy of crpc.dll
# because a copy command attached to the executable only runs when that
# executable relinks. In the common loop "edit the library, rebuild" only
# crpc.dll is relinked, so a stale copy would survive beside the test and the
# suite could pass against the old DLL.
#
# The scripts only ever select single-configuration generators (Ninja, or NMake
# Makefiles as a fallback), so the build root is exactly the directory above and
# `scripts/test.ps1` / `scripts/test.sh` look for the executables there. A
# multi-configuration generator (Visual Studio) would append `/<CONFIG>`
# automatically, so this setting would still be right for the build itself, but
# those generators are not supported by the scripts: the probe path would not
# resolve. Use `cmake --build <dir> --config <cfg>` directly in that case.

file(GLOB CRPC_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*/test_*.cpp")

# One executable per top-level test file: test_<area>.cpp becomes
# crpc_test_<area>. A nested file tests/<area>/test_<case>.cpp joins the
# executable of that area when it exists, otherwise it gets its own.
#
# test_codec, test_mvc and the ORM suite are skipped without the RTTI
# dependency: they use RTTI_DEFINE_TYPE / CRPC_ORM_ENTITY unconditionally and do
# not compile when CRPC_HAVE_RTTI is off. Every other test builds and runs in
# both cases, so this is a per-target guard rather than a hard requirement on
# the whole suite.
set(CRPC_RTTI_DEPENDENT_TESTS test_codec test_mvc test_orm_metadata test_orm_sql
                              test_orm_session test_orm_value test_orm_mysql_it)
if(NOT CRPC_HAVE_RTTI)
    message(STATUS "CRPC: RTTI dependency absent, skipping ${CRPC_RTTI_DEPENDENT_TESTS}")
endif()

foreach(source ${CRPC_TEST_SOURCES})
    get_filename_component(file_name "${source}" NAME_WE)
    get_filename_component(dir_name "${source}" DIRECTORY)
    get_filename_component(dir_name "${dir_name}" NAME)

    # test_shared_link is the shared-build consumer gate. It is registered
    # explicitly below, and only when CRPC_BUILD_SHARED=ON: in a static build
    # every symbol is in the archive by definition, so the probe would not
    # test anything the other targets do not already cover.
    if(file_name STREQUAL "test_shared_link")
        continue()
    endif()

    if(NOT CRPC_HAVE_RTTI AND file_name IN_LIST CRPC_RTTI_DEPENDENT_TESTS)
        continue()
    endif()

    if(dir_name STREQUAL "tests")
        set(target "crpc_${file_name}")
        set(sources "${source}")
    else()
        set(target "crpc_test_${dir_name}_${file_name}")
        set(sources "${source}")
    endif()

    add_executable(${target} ${sources})
    target_link_libraries(${target} PRIVATE crpc_core)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 30)
endforeach()

set_tests_properties(crpc_test_server_client PROPERTIES TIMEOUT 120)

# --- C ABI smoke test --------------------------------------------------------
# Compiled as C and linked against the (C++) library. LINKER_LANGUAGE CXX is
# required because the library needs the C++ runtime at link time.
add_executable(crpc_test_c_api "${CMAKE_CURRENT_SOURCE_DIR}/test_c_api.c")
set_target_properties(crpc_test_c_api PROPERTIES
    LINKER_LANGUAGE CXX
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_link_libraries(crpc_test_c_api PRIVATE crpc_core)
add_test(NAME crpc_test_c_api COMMAND crpc_test_c_api)
set_tests_properties(crpc_test_c_api PROPERTIES TIMEOUT 30)

# --- Shared-build link gate --------------------------------------------------
#
# A shared build only works if every public symbol a consumer can reference is
# in the export table. That is not something a runtime assertion can check: the
# failure mode is an unresolved external at *consumer* link time, and it can be
# reached through header-inline templates that no hand-written call list
# mentions (client_detail::wake_functor calls call_core::signal; mvc's
# bind_request calls route_params::find and header_list::find/add).
#
# Building this target against the shared library turns that into a build
# failure. It is registered for CRPC_BUILD_SHARED=ON in every configuration,
# including a no-RTTI one: the reflection DTO inside it is guarded by
# `#if CRPC_HAVE_RTTI` and the one case that needs it skips, so the export
# check itself survives. Registering it only when RTTI is present would leave a
# no-RTTI shared build with no runtime export gate at all -- and a symbol no
# test references would then go missing with no warning.
if(CRPC_BUILD_SHARED)
    add_executable(crpc_test_shared_link
        "${CMAKE_CURRENT_SOURCE_DIR}/test_shared_link.cpp")
    target_link_libraries(crpc_test_shared_link PRIVATE crpc_core)
    target_include_directories(crpc_test_shared_link
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Same placement as every other test: the executable sits in the build root
    # next to crpc.dll, so the import resolves from its own directory.
    set_target_properties(crpc_test_shared_link PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME crpc_test_shared_link COMMAND crpc_test_shared_link)
    set_tests_properties(crpc_test_shared_link PROPERTIES TIMEOUT 60)
endif()
