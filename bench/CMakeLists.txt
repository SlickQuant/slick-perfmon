# A measuring tool, so it is only worth building optimized - and the targets
# below force that on whatever the configuration selects.
#
# In a Debug configuration that collides rather than merely overriding: /Od
# loses to the /O2 that follows it, but MSVC refuses /O2 together with /RTC1
# outright - D8016, an error rather than a choice - so the runtime checks have
# to go with it. Stripped for this directory only, which contains nothing but
# the benchmarks; every other target keeps its configuration's own flags.
#
# This makes the binary *build* in a Debug configuration, which is all it does.
# The debug C runtime still costs roughly 3x per stamp, so those numbers are not
# the ones the README quotes - perfmon_bench warns at startup rather than
# leaving that to be noticed.
if(MSVC)
    foreach(flags CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "/RTC[1csu]+" "" ${flags} "${${flags}}")
    endforeach()
endif()

# One definition of "how a benchmark is built", because there are three of them
# and a flag that reached two would make the third quietly incomparable - which
# is the whole thing these binaries exist to avoid.
function(slick_perfmon_add_bench name)
    add_executable(${name} perfmon_bench.cpp)
    target_link_libraries(${name} PRIVATE slick::perfmon)
    if(MSVC)
        # C4324: structures padded by alignas, deliberate in the sample record.
        target_compile_options(${name} PRIVATE /O2 /DNDEBUG /wd4324)
    else()
        target_compile_options(${name} PRIVATE -O3 -march=native -DNDEBUG)
    endif()
endfunction()

slick_perfmon_add_bench(perfmon_bench)

# The compile-time-disabled case has to be its own binary: proving a stamp
# costs nothing means compiling one that is not there.
slick_perfmon_add_bench(perfmon_bench_disabled)
target_compile_definitions(perfmon_bench_disabled PRIVATE SLICK_PERFMON_ENABLED=0)

# Fences are most of the cost of a stamp, so the unfenced configuration needs
# its own binary to be measurable at all - SLICK_PERFMON_SERIALIZE is a
# compile-time switch.
slick_perfmon_add_bench(perfmon_bench_unfenced)
target_compile_definitions(perfmon_bench_unfenced PRIVATE SLICK_PERFMON_SERIALIZE=0)
