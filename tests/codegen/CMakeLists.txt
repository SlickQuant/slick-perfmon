# Compiles stamp_order.cpp, then checks the code it produced. Flags are the
# same in every configuration, so a Debug tree checks optimised code too: the
# per-config ones go because /Od and /Ob0 would void the check and MSVC rejects
# /O2 beside /RTC1. Directory scope, so the other tests keep theirs.
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_CXX_FLAGS_${cfg} "")
endforeach()

add_library(slick-perfmon-stamp-order OBJECT stamp_order.cpp)
target_link_libraries(slick-perfmon-stamp-order PRIVATE slick::perfmon)
set(object $<TARGET_OBJECTS:slick-perfmon-stamp-order>)

if(MSVC)
    # The test disassembles the object: a cl listing at a set path (/Fa<file>)
    # fails to open under MSBuild 18, and dumpbin runs without vcvars.
    get_filename_component(cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(link_dir "${CMAKE_LINKER}" DIRECTORY)
    find_program(SLICK_PERFMON_DUMPBIN dumpbin HINTS "${cl_dir}" "${link_dir}")
    target_compile_options(slick-perfmon-stamp-order PRIVATE /O2)
    set(input -DOBJECT=${object} -DDUMPBIN=${SLICK_PERFMON_DUMPBIN})
else()
    # -S wins over CMake's -c, so the "object" is the listing.
    target_compile_options(slick-perfmon-stamp-order PRIVATE -O2 -S)
    set(input -DLISTING=${object})
endif()

add_test(NAME slick-perfmon-stamp_order
         COMMAND ${CMAKE_COMMAND} ${input}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_stamp_order.cmake)
set_tests_properties(slick-perfmon-stamp_order PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
