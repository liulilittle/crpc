# CRPC test suite.
#
# Tests are discovered by pattern so that adding a test file never requires
# editing this file. That keeps parallel work on independent subsystems free of
# merge conflicts on the build script. CONFIGURE_DEPENDS makes CMake re-glob
# when a file is added or removed, so a stale configure is not a trap.

# The C ABI smoke test is a real C translation unit (not C++ compiled with a C
# driver), so the C language must be enabled for this directory.
enable_language(C)

# In a shared build every test executable imports crpc.dll, which CMake writes
# to the build root. Windows resolves an import from the executable's own
# directory and then the current directory; it does not search the parent of
# the executable's directory, so a test staged into <build>/tests/ dies before
# main() with STATUS_DLL_NOT_FOUND (0xC0000135). `crpc_copy_dll_beside` is
# defined by the top-level CMakeLists.txt (inside its CRPC_BUILD_SHARED block)
# and is shared by every directory that creates executables.

file(GLOB CRPC_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*/test_*.cpp")

# One executable per top-level test file: test_<area>.cpp becomes
# crpc_test_<area>. A nested file tests/<area>/test_<case>.cpp joins the
# executable of that area when it exists, otherwise it gets its own.
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
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 30)
    if(CRPC_BUILD_SHARED)
        crpc_copy_dll_beside(${target})
    endif()
endforeach()

set_tests_properties(crpc_test_server_client PROPERTIES TIMEOUT 120)

# --- C ABI smoke test --------------------------------------------------------
# Compiled as C and linked against the (C++) library. LINKER_LANGUAGE CXX is
# required because the library needs the C++ runtime at link time.
add_executable(crpc_test_c_api "${CMAKE_CURRENT_SOURCE_DIR}/test_c_api.c")
set_target_properties(crpc_test_c_api PROPERTIES
    LINKER_LANGUAGE CXX
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
target_link_libraries(crpc_test_c_api PRIVATE crpc_core)
add_test(NAME crpc_test_c_api COMMAND crpc_test_c_api)
set_tests_properties(crpc_test_c_api PROPERTIES TIMEOUT 30)
if(CRPC_BUILD_SHARED)
    crpc_copy_dll_beside(crpc_test_c_api)
endif()

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
# failure. It is registered only for CRPC_BUILD_SHARED=ON; a static build has no
# export table to get wrong, and docs/abi.md section 3 records the rule.
if(CRPC_BUILD_SHARED)
    add_executable(crpc_test_shared_link
        "${CMAKE_CURRENT_SOURCE_DIR}/test_shared_link.cpp")
    target_link_libraries(crpc_test_shared_link PRIVATE crpc_core)
    target_include_directories(crpc_test_shared_link
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME crpc_test_shared_link COMMAND crpc_test_shared_link)
    # Same DLL lookup as every other test: the import is resolved from the
    # executable's own directory, so the probe must sit next to crpc.dll.
    crpc_copy_dll_beside(crpc_test_shared_link)
    set_tests_properties(crpc_test_shared_link PROPERTIES TIMEOUT 60)
endif()
