// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#pragma once

#include <slick/perfmon/config.hpp>

#include <chrono>
#include <cstdint>

#if SLICK_PERFMON_X86
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <cpuid.h>
        #include <x86intrin.h>
    #endif
#endif

namespace slick::perfmon::detail {

// ============================================================================
// Reading the counter
//
// The bracket is Intel's: open with LFENCE; RDTSC; LFENCE, close with
// RDTSCP; LFENCE. RDTSCP alone waits for prior instructions to retire but does
// nothing to stop *later* work drifting up above it, which is what the trailing
// LFENCE is for. An intermediate stamp in a multi-step span is simultaneously
// the close of one stage and the open of the next, so it uses the closing form.
// ============================================================================

#if SLICK_PERFMON_X86

/// Opening stamp: nothing before it may still be in flight, and nothing after
/// it may run early.
SLICK_PERFMON_FORCE_INLINE uint64_t rdtsc_begin() noexcept {
#if SLICK_PERFMON_SERIALIZE
    _mm_lfence();
    const uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
#else
    return __rdtsc();
#endif
}

/// Closing or intermediate stamp. RDTSCP retires everything before it; the
/// LFENCE keeps everything after it from moving up.
SLICK_PERFMON_FORCE_INLINE uint64_t rdtsc_step() noexcept {
    unsigned aux;
#if SLICK_PERFMON_SERIALIZE
    const uint64_t t = __rdtscp(&aux);
    _mm_lfence();
#else
    const uint64_t t = __rdtscp(&aux);
#endif
    (void)aux;
    return t;
}

/// CPUID.0x80000007:EDX[8]. Without an invariant TSC the counter changes rate
/// with P-states and stops in deep C-states, so every number this library
/// produces would be fiction. We report it rather than refusing to run.
inline bool probe_invariant_tsc() noexcept {
#if defined(_MSC_VER)
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(regs[0]) < 0x80000007u) {
        return false;
    }
    __cpuid(regs, static_cast<int>(0x80000007u));
    return (static_cast<unsigned>(regs[3]) & (1u << 8)) != 0;
#else
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) || eax < 0x80000007u) {
        return false;
    }
    if (!__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    return (edx & (1u << 8)) != 0;
#endif
}

#else  // !SLICK_PERFMON_X86

// steady_clock is correct everywhere but costs roughly an order of magnitude
// more than RDTSC, so the documented per-stamp budget does not hold here. The
// warning exists so nobody quotes x86 overhead numbers from an ARM build.
#if defined(_MSC_VER)
    #pragma message("slick-perfmon: no cycle counter on this architecture; " \
                    "falling back to steady_clock. Overhead figures do not apply.")
#else
    #warning "slick-perfmon: no cycle counter on this architecture; " \
             "falling back to steady_clock. Overhead figures do not apply."
#endif

SLICK_PERFMON_FORCE_INLINE uint64_t rdtsc_begin() noexcept {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

SLICK_PERFMON_FORCE_INLINE uint64_t rdtsc_step() noexcept { return rdtsc_begin(); }

inline bool probe_invariant_tsc() noexcept { return true; }

#endif  // SLICK_PERFMON_X86

/// Dispatch used by stamp<>(). A template parameter rather than a runtime test
/// on the step value: with a runtime `seq` the caller's event_id is not a
/// constant, so a `step == kBeginStep` branch would survive into the hot path.
template <bool IsBegin>
SLICK_PERFMON_FORCE_INLINE uint64_t read_tsc() noexcept {
    if constexpr (IsBegin) {
        return rdtsc_begin();
    } else {
        return rdtsc_step();
    }
}

// ============================================================================
// Calibration
// ============================================================================

/**
 * @brief Cycles per second, derived by watching the TSC against steady_clock.
 *
 * QueryPerformanceFrequency is not the TSC rate on Windows - QPC usually ticks
 * at 10 MHz regardless of the CPU - and /proc/cpuinfo reports the nominal
 * frequency, which is not what the counter runs at on every part. So the rate
 * is measured, and then refined for free: every flush hands us a fresh
 * (tsc, wall) pair, and re-deriving over the whole run rather than the last
 * interval makes the estimate better the longer the process lives.
 */
class tsc_clock {
public:
    /// Blocking initial estimate. Runs on the collector thread, never on the
    /// caller's, so a 10 ms calibration does not delay start().
    void calibrate(std::chrono::milliseconds window) noexcept {
        invariant_ = probe_invariant_tsc();

        base_tsc_  = rdtsc_begin();
        base_wall_ = std::chrono::steady_clock::now();

        const auto deadline = base_wall_ + window;
        // Spin rather than sleep: a sleep would hand back control for a
        // scheduler quantum, and on Windows the ~15.6 ms timer granularity
        // would make a 10 ms window unreproducible.
        while (std::chrono::steady_clock::now() < deadline) {
        }

        const uint64_t t1 = rdtsc_step();
        const auto     w1 = std::chrono::steady_clock::now();
        update(t1, w1);
    }

    /// Fold one more (tsc, wall) pair into the estimate. The baseline is the
    /// whole run, so accuracy improves monotonically without any state beyond
    /// the two anchors.
    void update(uint64_t now_tsc, std::chrono::steady_clock::time_point now_wall) noexcept {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            now_wall - base_wall_).count();
        if (ns <= 0 || now_tsc <= base_tsc_) {
            return;
        }
        const double cycles = static_cast<double>(now_tsc - base_tsc_);
        hz_ = cycles * 1e9 / static_cast<double>(ns);
    }

    /// Cycles per second. Zero until calibrate() has run.
    double hz() const noexcept { return hz_; }

    bool invariant() const noexcept { return invariant_; }

    /// Cycles -> nanoseconds. Returns 0 before calibration rather than dividing
    /// by zero; the collector never formats a row before calibrating.
    double to_ns(double cycles) const noexcept {
        return hz_ > 0.0 ? cycles * 1e9 / hz_ : 0.0;
    }

private:
    uint64_t                              base_tsc_  = 0;
    std::chrono::steady_clock::time_point base_wall_{};
    double                                hz_        = 0.0;
    bool                                  invariant_ = true;
};

}  // namespace slick::perfmon::detail
