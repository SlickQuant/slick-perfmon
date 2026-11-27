# Checks the code compiled from stamp_order.cpp: in each probe, the listed
# tokens must appear in this order. Usage:
#   cmake -DLISTING=<GCC/Clang -S output> -P <this>
#   cmake -DOBJECT=<MSVC .obj> -DDUMPBIN=<dumpbin.exe> -P <this>
#
# Text order stands in for execution order, which holds because every probe is
# straight-line up to the closing stamp.

cmake_minimum_required(VERSION 3.21)

if(DEFINED OBJECT)
    if(NOT DUMPBIN)
        message("SKIPPED: no dumpbin to disassemble the object with")
        return()
    endif()
    execute_process(COMMAND "${DUMPBIN}" /nologo /disasm "${OBJECT}"
                    OUTPUT_VARIABLE text ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "dumpbin failed (${rc}) on '${OBJECT}':\n${text}${err}")
    endif()
    set(fn_end "^$")  # dumpbin ends each function with a blank line
else()
    if(NOT EXISTS "${LISTING}")
        message(FATAL_ERROR "no listing at '${LISTING}'")
    endif()
    file(READ "${LISTING}" text)
    set(fn_end "\\.cfi_endproc|\\.seh_endproc|^[ \t]*\\.size")
endif()

# Neutralise what CMake lists treat specially before splitting into lines.
string(REPLACE "\r" "" text "${text}")
string(REPLACE "\\" "/" text "${text}")
string(REPLACE ";" "#" text "${text}")
string(REPLACE "[" "(" text "${text}")
string(REPLACE "]" ")" text "${text}")
string(REPLACE "\n" ";" lines "${text}")

set(fn "")
foreach(line IN LISTS lines)
    if(line MATCHES "^_?(slick_probe_[a-z_]+):")
        set(fn ${CMAKE_MATCH_1})
        set(body_${fn} "")
        list(APPEND probes ${fn})
    elseif(fn AND line MATCHES "${fn_end}")
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
# Stores of 0x5EED000n: decimal in GCC/Clang output, 5EED000nh from dumpbin.
foreach(n RANGE 3)
    math(EXPR v "0x5EED0000 + ${n}")
    set(m${n} "[^0-9](${v}|5EED000${n}h)([^0-9]|$)")
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
