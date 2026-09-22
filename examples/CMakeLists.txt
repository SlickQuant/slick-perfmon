set(SLICK_PERFMON_EXAMPLES
    basic_example
    pipeline_example
    multithread_example
    shm_producer_example
)

foreach(example IN LISTS SLICK_PERFMON_EXAMPLES)
    add_executable(${example} ${example}.cpp)
    target_include_directories(${example} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${example} PRIVATE slick::perfmon)
    if(MSVC)
        target_compile_options(${example} PRIVATE /wd4324)
    endif()
endforeach()
