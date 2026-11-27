# Compiles stamp_order.cpp to an assembly listing, then checks it. Flags are the
# same in every configuration, so a Debug tree checks optimised code too: the
# per-config ones go because /Od and /Ob0 would void the check and MSVC rejects
# /O2 beside /RTC1. Directory scope, so the other tests keep theirs.
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_CXX_FLAGS_${cfg} "")
endforeach()

add_library(slick-perfmon-stamp-order OBJECT stamp_order.cpp)
target_link_libraries(slick-perfmon-stamp-order PRIVATE slick::perfmon)

if(MSVC)
    set(listing ${CMAKE_CURRENT_BINARY_DIR}/stamp_order.asm)
    target_compile_options(slick-perfmon-stamp-order PRIVATE /O2 /FA /Fa${listing})
else()
    # -S wins over CMake's -c, so the "object" is the listing.
    set(listing $<TARGET_OBJECTS:slick-perfmon-stamp-order>)
    target_compile_options(slick-perfmon-stamp-order PRIVATE -O2 -S)
endif()

add_test(NAME slick-perfmon-stamp_order
         COMMAND ${CMAKE_COMMAND} -DLISTING=${listing}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_stamp_order.cmake)
set_tests_properties(slick-perfmon-stamp_order PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
