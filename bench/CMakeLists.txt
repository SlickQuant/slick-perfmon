# A measuring tool, so it is only worth building optimized.
add_executable(perfmon_bench perfmon_bench.cpp)
target_link_libraries(perfmon_bench PRIVATE slick::perfmon)

if(MSVC)
    target_compile_options(perfmon_bench PRIVATE /O2 /DNDEBUG /wd4324)
else()
    target_compile_options(perfmon_bench PRIVATE -O3 -march=native -DNDEBUG)
endif()

# The compile-time-disabled case has to be its own binary: proving a stamp
# costs nothing means compiling one that is not there.
add_executable(perfmon_bench_disabled perfmon_bench.cpp)
target_link_libraries(perfmon_bench_disabled PRIVATE slick::perfmon)
target_compile_definitions(perfmon_bench_disabled PRIVATE SLICK_PERFMON_ENABLED=0)
if(MSVC)
    target_compile_options(perfmon_bench_disabled PRIVATE /O2 /DNDEBUG /wd4324)
else()
    target_compile_options(perfmon_bench_disabled PRIVATE -O3 -march=native -DNDEBUG)
endif()

# Fences are most of the cost of a stamp, so the unfenced configuration needs
# its own binary to be measurable at all - SLICK_PERFMON_SERIALIZE is a
# compile-time switch.
add_executable(perfmon_bench_unfenced perfmon_bench.cpp)
target_link_libraries(perfmon_bench_unfenced PRIVATE slick::perfmon)
target_compile_definitions(perfmon_bench_unfenced PRIVATE SLICK_PERFMON_SERIALIZE=0)
if(MSVC)
    target_compile_options(perfmon_bench_unfenced PRIVATE /O2 /DNDEBUG /wd4324)
else()
    target_compile_options(perfmon_bench_unfenced PRIVATE -O3 -march=native -DNDEBUG)
endif()
