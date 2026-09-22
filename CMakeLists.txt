# 3.21 is the real floor: PROJECT_IS_TOP_LEVEL below is 3.21+, and
# FetchContent_MakeAvailable is 3.14+.
cmake_minimum_required(VERSION 3.21)

project(slick-perfmon
        VERSION 0.1.0
        DESCRIPTION "A C++20 low-latency RDTSC performance measurement library"
        LANGUAGES CXX)

# Feature switches. Both ride on the INTERFACE target and are baked into the
# installed Config.cmake as the value they had *here*, so a downstream
# find_package() consumer gets the headers this build configured - not whatever
# the same-named variable happens to hold in their own cache. A consumer wanting
# a different answer defines SLICK_PERFMON_ENABLED=0 on their own target.
option(SLICK_PERFMON_ENABLED "Compile instrumentation in (OFF makes every stamp a no-op)" ON)
option(SLICK_PERFMON_SERIALIZE "Fence the timestamp reads (OFF is faster but reorderable)" ON)

option(BUILD_SLICK_PERFMON_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(BUILD_SLICK_PERFMON_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
option(BUILD_SLICK_PERFMON_COLLECTOR "Build the standalone collector executable"
       ${PROJECT_IS_TOP_LEVEL})
# Off by default: the benchmark is a measuring tool, not part of a normal build.
option(BUILD_SLICK_PERFMON_BENCH "Build benchmarks" OFF)

find_package(Threads REQUIRED)

# Both of these back the ring and the control block, and both are reachable only
# from headers behind `#if SLICK_PERFMON_ENABLED`. Finding them anyway would make
# a disabled build require - and a disabled *package* impose on its consumers -
# two dependencies whose code it does not contain, contradicting the whole point
# of the switch.
if(SLICK_PERFMON_ENABLED)
    find_package(slick-queue 2.0.0 CONFIG QUIET)
    if (NOT slick-queue_FOUND)
        message(STATUS "Fetching slick-queue...")
        include(FetchContent)
        set(BUILD_SLICK_QUEUE_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            slick-queue
            GIT_REPOSITORY https://github.com/SlickQuant/slick-queue.git
            GIT_TAG v2.0.0 # See https://github.com/SlickQuant/slick-queue/releases for latest version
        )
        FetchContent_MakeAvailable(slick-queue)
    else()
        message(STATUS "Found slick-queue ${slick-queue_VERSION}: ${slick-queue_DIR}")
    endif()

    # slick-queue pulls this in transitively, but slick-perfmon uses it directly
    # for the control block, so it is a first-class dependency here.
    find_package(slick-shm CONFIG QUIET)
    if (NOT slick-shm_FOUND)
        message(STATUS "Fetching slick-shm...")
        include(FetchContent)
        set(SLICK_SHM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SLICK_SHM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            slick-shm
            GIT_REPOSITORY https://github.com/SlickQuant/slick-shm.git
            GIT_TAG v0.1.6 # See https://github.com/SlickQuant/slick-shm/releases for latest version
        )
        FetchContent_MakeAvailable(slick-shm)
    else()
        message(STATUS "Found slick-shm ${slick-shm_VERSION}: ${slick-shm_DIR}")
    endif()
else()
    message(STATUS
        "slick-perfmon: instrumentation disabled - skipping slick-queue and slick-shm")
endif()

# version.hpp is generated into the source tree so the headers stay usable
# without configuring (a direct copy of include/ works), matching slick-logger.
configure_file(cmake/version.hpp.in
               ${CMAKE_CURRENT_SOURCE_DIR}/include/slick/perfmon/version.hpp
               @ONLY NEWLINE_STYLE LF)

add_library(slick-perfmon INTERFACE)
add_library(slick::perfmon ALIAS slick-perfmon)

target_include_directories(slick-perfmon INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(slick-perfmon PROPERTIES EXPORT_NAME perfmon)

# The requirement must ride on the target, not on CMAKE_CXX_STANDARD: that
# variable applies only to targets built here, so an installed consumer would
# compile these headers under whatever standard it happens to use. slick-queue
# makes exactly that mistake, which is why this is spelled out rather than
# inherited.
target_compile_features(slick-perfmon INTERFACE cxx_std_20)

# Threads unconditionally: the collector thread is compiled out with the rest,
# but Threads::Threads is a system requirement rather than a package a consumer
# has to go and install, and keeping it removes a conditional from the export.
target_link_libraries(slick-perfmon INTERFACE Threads::Threads)

if(SLICK_PERFMON_ENABLED)
    target_link_libraries(slick-perfmon INTERFACE slick::queue slick::shm)
    if(UNIX AND NOT APPLE)
        # Wanted by the shm segments, which a disabled build never maps.
        target_link_libraries(slick-perfmon INTERFACE rt atomic)
    endif()
endif()
if(WIN32)
    target_compile_definitions(slick-perfmon INTERFACE _WIN32_WINNT=0x0A00)
endif()

if(NOT SLICK_PERFMON_ENABLED)
    target_compile_definitions(slick-perfmon INTERFACE SLICK_PERFMON_ENABLED=0)
endif()
if(NOT SLICK_PERFMON_SERIALIZE)
    target_compile_definitions(slick-perfmon INTERFACE SLICK_PERFMON_SERIALIZE=0)
endif()

if(BUILD_SLICK_PERFMON_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_SLICK_PERFMON_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_SLICK_PERFMON_COLLECTOR)
    add_subdirectory(tools)
endif()

if(BUILD_SLICK_PERFMON_BENCH)
    add_subdirectory(bench)
endif()

# ----------------------------------------------------------------------------
# Installation
# ----------------------------------------------------------------------------
include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Generate and install CMake config files for vcpkg compatibility
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/slick-perfmonConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/slick-perfmonConfig.cmake"
    INSTALL_DESTINATION lib/cmake/slick-perfmon
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/slick-perfmonConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/slick-perfmonConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/slick-perfmonConfigVersion.cmake"
    DESTINATION lib/cmake/slick-perfmon
)

install(TARGETS slick-perfmon EXPORT slick-perfmonTargets)

install(EXPORT slick-perfmonTargets
    FILE slick-perfmonTargets.cmake
    NAMESPACE slick::
    DESTINATION lib/cmake/slick-perfmon
)

message(STATUS "${PROJECT_NAME}: ${PROJECT_VERSION}")
