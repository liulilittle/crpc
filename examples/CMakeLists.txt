# CRPC examples.
#
# Examples are built only when CRPC_BUILD_EXAMPLES=ON. They are consumers of the
# public API, not part of the unit-test suite: the quickstart is registered in
# CTest so a build proves it actually runs, but it is not a behavioural gate on
# the library the way tests/ is. They are still built with the same warning
# policy as the library, so example code cannot rot.
#
# The executables are written to the build root, next to crpc.dll (shared) and
# crpc.lib (static), for the same reason tests/ and bench/ are: Windows resolves
# an import from the executable's own directory and never searches its parent,
# so staging them into <build>/examples/ would need a copy of the DLL that an
# incremental library rebuild would leave stale.

# `quickstart*` are API tours. `deploy_check` is not a tour: it is the
# cross-process deployment gate, and it is here because it has to link as a
# consumer of the public API exactly like a real program would. Everything in
# this directory must follow the same registration rules (built at the build
# root, registered with CTest, exiting non-zero on failure), so it is picked up
# by the same glob rather than by its own target.
file(GLOB CRPC_EXAMPLE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/quickstart*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deploy_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/registryd.cpp")

foreach(source ${CRPC_EXAMPLE_SOURCES})
    get_filename_component(file_name "${source}" NAME_WE)
    set(target "crpc_example_${file_name}")
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE crpc)
    # A multi-configuration generator appends `/<CONFIG>` to this directory
    # automatically, so the plain form is correct for those too.
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 60)
endforeach()
