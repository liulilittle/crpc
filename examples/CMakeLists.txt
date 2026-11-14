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
# consumer of the public API exactly like a real program would.
#
# `registryd` is a daemon, not a one-shot case: it requires `--state-dir` /
# `--token`, refuses to start without them, and serves until stopped, so it can
# never be registered as a CTest entry that runs with no arguments. It is built
# like every other example (same warning policy) but deliberately NOT registered
# in CTest; its behaviour is covered by tests/test_registryd_composition.cpp
# (service + raft + HTTP) and by the console-signal probes under
# .opencode/tmp/. Everything else in this directory follows the same registration
# rules (built at the build root, registered with CTest, exiting non-zero on
# failure), so it is picked up by the same glob rather than by its own target.
file(GLOB CRPC_EXAMPLE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/quickstart*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/deploy_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/registryd.cpp")

foreach(source ${CRPC_EXAMPLE_SOURCES})
    get_filename_component(file_name "${source}" NAME_WE)
    set(target "crpc_example_${file_name}")
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE crpc)
    # registryd drives the raft driver with `std::jthread`/`std::stop_token`
    # (D-B-10 RAII guard), which are C++20. The library's `cxx_std_17` is a
    # floor, not a uniform standard, and the project default is C++17, so this
    # target has to declare its own requirement. Without it every default
    # (cxx17) examples build fails with C2039/STL4038. Only this target is
    # upgraded: the other examples stay at the project default.
    if(file_name STREQUAL "registryd")
        target_compile_features(${target} PRIVATE cxx_std_20)
    endif()
    # A multi-configuration generator appends `/<CONFIG>` to this directory
    # automatically, so the plain form is correct for those too.
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    # registryd is a daemon that needs arguments and runs until stopped; a
    # no-argument CTest entry would always fail (it prints usage and exits 2).
    if(NOT file_name STREQUAL "registryd")
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES TIMEOUT 60)
    endif()
endforeach()
