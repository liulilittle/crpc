# CRPC test suite.
#
# Tests are discovered by pattern so that adding a test file never requires
# editing this file. That keeps parallel work on independent subsystems free of
# merge conflicts on the build script. CONFIGURE_DEPENDS makes CMake re-glob
# when a file is added or removed, so a stale configure is not a trap.

# The C ABI smoke test is a real C translation unit (not C++ compiled with a C
# driver), so the C language must be enabled for this directory.
enable_language(C)

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
    set_tests_properties(${target} PROPERTIES TIMEOUT 300)
endforeach()

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
set_tests_properties(crpc_test_c_api PROPERTIES TIMEOUT 60)
