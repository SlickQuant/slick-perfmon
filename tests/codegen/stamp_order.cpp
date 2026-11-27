// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// Compiled to assembly, never run. check_stamp_order.cmake reads the listing
// and checks each marked piece of work sits between the stamps timing it. A
// span whose work the compiler moved out still returns a plausible number, so
// no runtime test can see this.

#include <slick/perfmon.hpp>

#include <cstdint>
#include <type_traits>

using namespace slick::perfmon;

#if SLICK_PERFMON_ENABLED && SLICK_PERFMON_X86 && SLICK_PERFMON_SERIALIZE

// Each store overwrites the last, so without a compiler barrier at a stamp the
// earlier one is dead and goes, or sinks across it. Every edge of both stamp
// kinds is covered. Expected: 0x5EED0000, rdtsc, 0x5EED0001, rdtscp,
// 0x5EED0002, rdtscp, 0x5EED0003.
extern "C" uint64_t slick_probe_stores(int* p) {
    *p = 0x5EED0000;
    const uint64_t t0 = detail::rdtsc_begin();
    *p = 0x5EED0001;
    const uint64_t t1 = detail::rdtsc_step();
    *p = 0x5EED0002;
    const uint64_t t2 = detail::rdtsc_step();
    *p = 0x5EED0003;
    return t0 ^ t1 ^ t2;
}

// A register-only result read on one branch after the span. Every compiler
// computes it after RDTSCP unless do_not_optimize() pins it. Expected: rdtsc,
// div, rdtscp. T picks the overload (const T is the read-only one) and the
// register class. A macro, and two dependent divisions, because MSVC leaves
// the work in place when it arrives through an inlined lambda or is a single
// division, and the probe would then pass without the fix.
#define SLICK_PROBE_RESULT(name, T, U, expr)                                   \
    extern "C" uint64_t name(U x, U y, bool use, std::remove_const_t<T>* out) { \
        const uint64_t t0 = detail::rdtsc_begin();                             \
        T              h  = expr;                                              \
        do_not_optimize(h);                                                    \
        const uint64_t t1 = detail::rdtsc_step();                              \
        if (use) {                                                             \
            *out = h;                                                          \
        }                                                                      \
        return t1 - t0;                                                        \
    }

struct quot_rem {
    uint64_t q, r;
};

SLICK_PROBE_RESULT(slick_probe_gpr, uint64_t, uint64_t, x / y / (y + 3))
SLICK_PROBE_RESULT(slick_probe_gpr_const, const uint64_t, uint64_t, x / y / (y + 3))
SLICK_PROBE_RESULT(slick_probe_xmm, double, double, x / y / (y + 3))
SLICK_PROBE_RESULT(slick_probe_mem, const quot_rem, uint64_t, (quot_rem{x / y / (y + 3), x % y}))

#else

// Nothing to check: no TSC, unfenced stamps, or instrumentation compiled out.
extern "C" int slick_probe_skipped() { return 0; }

#endif
