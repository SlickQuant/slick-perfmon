# Checks the assembly listing of stamp_order.cpp: in each probe, the listed
# tokens must appear in this order. Usage: cmake -DLISTING=<file> -P <this>.
#
# Reads GCC/Clang (AT&T) and MSVC (MASM) listings. Text order stands in for
# execution order, which holds because every probe is straight-line up to the
# closing stamp.

cmake_minimum_required(VERSION 3.21)

if(NOT EXISTS "${LISTING}")
    message(FATAL_ERROR "no listing at '${LISTING}'")
endif()

# Neutralise what CMake lists treat specially before splitting into lines.
file(READ "${LISTING}" text)
string(REPLACE "\\" "/" text "${text}")
string(REPLACE ";" "#" text "${text}")
string(REPLACE "[" "(" text "${text}")
string(REPLACE "]" ")" text "${text}")
string(REPLACE "\n" ";" lines "${text}")

set(fn "")
foreach(line IN LISTS lines)
    if(line MATCHES "^_?(slick_probe_[a-z_]+)(:| PROC)")
        set(fn ${CMAKE_MATCH_1})
        set(body_${fn} "")
        list(APPEND probes ${fn})
    elseif(fn AND line MATCHES "ENDP|\\.cfi_endproc|\\.seh_endproc|^[ \t]*\\.size")
        set(fn "")
    elseif(fn)
        list(APPEND body_${fn} "${line}")
    endif()
endforeach()

if("slick_probe_skipped" IN_LIST probes)
    message("SKIPPED: stamps are unfenced, not RDTSC, or compiled out")
    return()
endif()

set(rdtsc  "[ \t]rdtsc([ \t]|$)")
set(rdtscp "[ \t]rdtscp([ \t]|$)")
set(div    "[ \t]v?div[a-z]*[ \t]")
# Stores of 0x5EED000n, which both syntaxes print in decimal.
foreach(n RANGE 3)
    math(EXPR v "0x5EED0000 + ${n}")
    set(m${n} "[^0-9]${v}([^0-9]|$)")
endforeach()

set(failed FALSE)

function(expect_order fn)
    list(LENGTH body_${fn} n)
    if(n EQUAL 0)
        message(SEND_ERROR "${fn}: not in the listing")
        set(failed TRUE PARENT_SCOPE)
        return()
    endif()
    set(at 0)
    foreach(tok IN LISTS ARGN)
        set(hit FALSE)
        while(at LESS n)
            list(GET body_${fn} ${at} line)
            math(EXPR at "${at} + 1")
            if(line MATCHES "${tok}")
                set(hit TRUE)
                break()
            endif()
        endwhile()
        if(NOT hit)
            string(REPLACE ";" "\n" dump "${body_${fn}}")
            message(SEND_ERROR "${fn}: '${tok}' missing or out of order. Listing:\n${dump}")
            set(failed TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(STATUS "${fn}: ok")
endfunction()

expect_order(slick_probe_stores ${m0} ${rdtsc} ${m1} ${rdtscp} ${m2} ${rdtscp} ${m3})
foreach(p gpr gpr_const xmm mem)
    expect_order(slick_probe_${p} ${rdtsc} ${div} ${rdtscp})
endforeach()

if(failed)
    message(FATAL_ERROR "work crossed a stamp; see above")
endif()
