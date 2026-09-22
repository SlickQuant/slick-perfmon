find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
    )
    # For Windows: prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

if(MSVC)
    # C4324: structures padded by alignas. Deliberate here and in slick-queue -
    # the ring pointer and the sample record are both cache-line placed.
    add_compile_options(/wd4996 /wd4324)
endif()

function(slick_perfmon_add_test test_file)
    get_filename_component(test_name ${test_file} NAME_WE)
    add_executable(slick-perfmon-${test_name} ${test_file})
    target_include_directories(slick-perfmon-${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(slick-perfmon-${test_name} PRIVATE slick::perfmon GTest::gtest_main)
    add_test(NAME slick-perfmon-${test_name} COMMAND slick-perfmon-${test_name})
endfunction()

# Backend and encoding logic. None of it depends on stamps actually being
# recorded, so it is worth running in every configuration - including the one
# with instrumentation compiled out, where these headers still have to compile
# and behave.
set(SLICK_PERFMON_LOGIC_TESTS
    test_event_id.cpp
    test_histogram.cpp
    test_accumulator.cpp
    test_open_map.cpp
    test_pairing.cpp
    test_seq.cpp
)

foreach(test_file IN LISTS SLICK_PERFMON_LOGIC_TESTS)
    slick_perfmon_add_test(${test_file})
endforeach()

# Proves the disabled build costs nothing. Always built, and always with the
# switch forced off regardless of how the project was configured - that is the
# whole point of it.
add_executable(slick-perfmon-test_disabled test_disabled.cpp)
target_include_directories(slick-perfmon-test_disabled PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(slick-perfmon-test_disabled PRIVATE slick::perfmon GTest::gtest_main)
target_compile_definitions(slick-perfmon-test_disabled PRIVATE SLICK_PERFMON_ENABLED=0)
add_test(NAME slick-perfmon-test_disabled COMMAND slick-perfmon-test_disabled)

if(SLICK_PERFMON_ENABLED)
    # These assert that measurement works, which it cannot when every stamp has
    # been compiled out. Registering them in a disabled build would turn a
    # correctly-working configuration into a wall of red.
    set(SLICK_PERFMON_MEASURING_TESTS
        test_scoped.cpp
        test_collector.cpp
        test_tsc.cpp
        test_output.cpp
        # Pure logic, but the shared name table is a shm segment, and a disabled
        # build deliberately does not depend on slick-shm at all - so this one
        # cannot be built there even though what it asserts still holds.
        test_names.cpp
    )

    foreach(test_file IN LISTS SLICK_PERFMON_MEASURING_TESTS)
        slick_perfmon_add_test(${test_file})
    endforeach()

    # Cross-process test: a real second process attaches the same segment. The
    # producer's path is baked in as a compile definition so the test never has
    # to guess where the build put it.
    add_executable(slick-perfmon-test-producer shm_producer/producer_main.cpp)
    target_link_libraries(slick-perfmon-test-producer PRIVATE slick::perfmon)

    slick_perfmon_add_test(test_shm.cpp)
    target_compile_definitions(slick-perfmon-test_shm PRIVATE
        PRODUCER_EXE_PATH="$<TARGET_FILE:slick-perfmon-test-producer>")
    add_dependencies(slick-perfmon-test_shm slick-perfmon-test-producer)
else()
    message(STATUS
        "slick-perfmon: instrumentation disabled - skipping the measurement tests")
endif()
