cmake_minimum_required(VERSION 3.21)
project(slick-perfmon-consumer LANGUAGES CXX)

# Configured against an installed package, never this source tree. Generation
# is where a bad INTERFACE_INCLUDE_DIRECTORIES fails - CMake refuses an imported
# target naming a directory that does not exist - and the build below proves the
# path it names is the one the headers were actually installed to.
find_package(slick-perfmon CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE slick::perfmon)
