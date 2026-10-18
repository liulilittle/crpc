# CRPC test suite.
#
# One executable per subsystem keeps the CTest output readable and lets a
# single failing area be rebuilt and re-run in isolation.

set(CRPC_TEST_TARGETS "")

function(crpc_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE crpc_core)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
    set(CRPC_TEST_TARGETS ${CRPC_TEST_TARGETS} ${name} PARENT_SCOPE)
endfunction()

crpc_add_test(crpc_test_endian test_endian.cpp)
crpc_add_test(crpc_test_wire test_wire.cpp)
