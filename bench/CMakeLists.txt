# CRPC benchmarks.
#
# Benchmarks are not tests: they are built only when CRPC_BUILD_BENCH=ON and are
# never part of the CI pass/fail gate (a shared runner's numbers are not
# comparable and must not turn a green build red). They are still built with the
# same warning policy so that benchmark code cannot rot.
#
# The executables are written to the build root, next to crpc.dll (shared) and
# crpc.lib (static). In a shared build the import is then resolved from the
# executable's own directory; Windows never searches the parent directory, so
# staging them into <build>/bench/ would need a copy of the DLL. A copy would be
# wrong under incremental builds: it only runs when the executable relinks, so a
# library-only rebuild would leave a stale crpc.dll in place.

file(GLOB CRPC_BENCH_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/bench_*.cpp")

foreach(source ${CRPC_BENCH_SOURCES})
    get_filename_component(file_name "${source}" NAME_WE)
    set(target "crpc_${file_name}")
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE crpc)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # A multi-configuration generator appends `/<CONFIG>` to this directory
    # automatically, so the plain form is correct for those too.
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()
