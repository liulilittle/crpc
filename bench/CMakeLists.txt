# CRPC benchmarks.
#
# Benchmarks are not tests: they are built only when CRPC_BUILD_BENCH=ON and are
# never part of the CI pass/fail gate (a shared runner's numbers are not
# comparable and must not turn a green build red). They are still built with the
# same warning policy so that benchmark code cannot rot.

file(GLOB CRPC_BENCH_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/bench_*.cpp")

foreach(source ${CRPC_BENCH_SOURCES})
    get_filename_component(file_name "${source}" NAME_WE)
    set(target "crpc_${file_name}")
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE crpc)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench")
    # A shared build stages these into <build>/bench/, one level below
    # crpc.dll, and the Windows loader does not search the parent directory.
    if(CRPC_BUILD_SHARED)
        crpc_copy_dll_beside(${target})
    endif()
endforeach()
