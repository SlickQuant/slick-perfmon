# Installs slick-perfmon under a non-default include directory and builds a
# find_package() consumer against the result.
#
# Regression: the exported target hardcoded `include` while the headers went to
# CMAKE_INSTALL_INCLUDEDIR, so any other value produced a package whose
# imported target named a directory that did not exist, and every consumer
# failed at generation.
#
# Run as `cmake -P` with SOURCE_DIR, WORK_DIR and GENERATOR set; PLATFORM,
# TOOLSET and CXX_COMPILER are passed on when the parent build has them.
#
# Packaged with instrumentation off: that build depends on nothing beyond
# Threads, so the test needs no network and no slick-queue or slick-shm install,
# and the include path it checks is the same one an enabled package exports.

foreach(var SOURCE_DIR WORK_DIR GENERATOR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "check_install.cmake: ${var} is not set")
    endif()
endforeach()

set(include_dir perfinclude)
set(build_dir    "${WORK_DIR}/build")
set(prefix       "${WORK_DIR}/prefix")
set(consumer_dir "${WORK_DIR}/consumer")
file(REMOVE_RECURSE "${WORK_DIR}")

set(generator_args -G "${GENERATOR}")
if(PLATFORM)
    list(APPEND generator_args -A "${PLATFORM}")
endif()
if(TOOLSET)
    list(APPEND generator_args -T "${TOOLSET}")
endif()
if(CXX_COMPILER)
    list(APPEND generator_args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()

function(run step)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${step} failed (${rc}):\n${out}")
    endif()
endfunction()

run("configure the package"
    "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${build_dir}" ${generator_args}
    -DSLICK_PERFMON_ENABLED=OFF
    -DBUILD_SLICK_PERFMON_TESTS=OFF
    -DBUILD_SLICK_PERFMON_EXAMPLES=OFF
    -DBUILD_SLICK_PERFMON_COLLECTOR=OFF
    -DBUILD_SLICK_PERFMON_BENCH=OFF
    "-DCMAKE_INSTALL_INCLUDEDIR=${include_dir}"
    "-DCMAKE_INSTALL_PREFIX=${prefix}")
run("install the package"
    "${CMAKE_COMMAND}" --install "${build_dir}" --config Release)

# The headers have to be where the variable said, and only there - a copy left
# under the default name would let a hardcoded path pass by accident.
if(NOT EXISTS "${prefix}/${include_dir}/slick/perfmon.hpp")
    message(FATAL_ERROR "headers were not installed to ${include_dir}")
endif()
if(EXISTS "${prefix}/include")
    message(FATAL_ERROR "something was installed to the default include directory")
endif()

run("configure the consumer"
    "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/consumer" -B "${consumer_dir}"
    ${generator_args} "-DCMAKE_PREFIX_PATH=${prefix}")
run("build the consumer"
    "${CMAKE_COMMAND}" --build "${consumer_dir}" --config Release)
