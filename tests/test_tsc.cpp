// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// Everything this library reports is cycles converted to nanoseconds, so a
// wrong TSC frequency makes every figure wrong by the same factor - silently.

#include <slick/perfmon.hpp>
#include <slick/perfmon/collector.hpp>
#include <slick/perfmon/detail/tsc.hpp>

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace slick::perfmon;
using namespace slick::perfmon::test;

namespace {
enum class pt : point_id { sleeper = 0, count };
}

TEST(Tsc, CounterAdvancesMonotonically) {
    const uint64_t a = detail::rdtsc_begin();
    const uint64_t b = detail::rdtsc_step();
    const uint64_t c = detail::rdtsc_begin();
    EXPECT_LE(a, b);
    EXPECT_LE(b, c);
    EXPECT_NE(a, 0u) << "zero is the disabled sentinel and must never be a real reading";
}

TEST(Tsc, InvariantProbeDoesNotCrash) {
    // The answer is whatever this CPU says; what matters is that the CPUID path
    // is safe on every machine, including ones without leaf 0x80000007.
    const bool invariant = detail::probe_invariant_tsc();
    SUCCEED() << "invariant TSC: " << (invariant ? "yes" : "no");
}

TEST(Tsc, CalibratesToAPlausibleFrequency) {
    detail::tsc_clock clock;
    clock.calibrate(std::chrono::milliseconds(20));

    const double hz = clock.hz();
    EXPECT_GT(hz, 0.5e9) << "below 500 MHz means the calibration is broken, not the CPU";
    EXPECT_LT(hz, 1.0e10) << "above 10 GHz means the calibration is broken, not the CPU";
}

TEST(Tsc, ConvertsCyclesToNanoseconds) {
    detail::tsc_clock clock;
    clock.calibrate(std::chrono::milliseconds(20));

    // One second worth of cycles must convert to one second of nanoseconds.
    EXPECT_NEAR(clock.to_ns(clock.hz()), 1e9, 1e9 * 0.001);
    EXPECT_DOUBLE_EQ(clock.to_ns(0.0), 0.0);
}

TEST(Tsc, AnUncalibratedClockReportsZeroRatherThanDividingByZero) {
    detail::tsc_clock clock;
    EXPECT_DOUBLE_EQ(clock.hz(), 0.0);
    EXPECT_DOUBLE_EQ(clock.to_ns(1000.0), 0.0);
}

TEST(Tsc, RefinementDoesNotDestabiliseTheEstimate) {
    detail::tsc_clock clock;
    clock.calibrate(std::chrono::milliseconds(10));
    const double first = clock.hz();

    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        clock.update(detail::rdtsc_step(), std::chrono::steady_clock::now());
    }

    // Refining over a longer baseline should tighten the estimate, never send
    // it somewhere else entirely.
    EXPECT_NEAR(clock.hz(), first, first * 0.05);
}

TEST(Tsc, UpdateIgnoresNonsenseInput) {
    detail::tsc_clock clock;
    clock.calibrate(std::chrono::milliseconds(10));
    const double before = clock.hz();

    // A backwards timestamp must not be folded in.
    clock.update(0, std::chrono::steady_clock::now());
    EXPECT_DOUBLE_EQ(clock.hz(), before);
}

TEST(Tsc, AMeasuredSleepConvertsBackToRealTime) {
    // The end-to-end check that matters: a span of known wall-clock duration
    // has to come back out of the pipeline as that duration.
    Collector c;
    config    cfg   = quiet_config();
    cfg.point_count = static_cast<point_id>(pt::count);
    cfg.calibration_time = std::chrono::milliseconds(20);
    ASSERT_TRUE(c.start(cfg));

    constexpr auto kSleep = std::chrono::milliseconds(50);
    begin(pt::sleeper);
    std::this_thread::sleep_for(kSleep);
    end(pt::sleeper);

    const stats s = c.snapshot_total(to_point(pt::sleeper));
    ASSERT_EQ(s.count, 1u);

    const double expected_ns = 50e6;

    // This test exists to catch a frequency that is wrong by orders of
    // magnitude - mistaking QPC's 10 MHz for the TSC's ~3 GHz would land ~300x
    // out - not to measure the scheduler. A sleep only ever guarantees "at
    // least", and on a loaded machine Windows will happily turn 50 ms into 80,
    // so the upper bound is deliberately loose. Tightening it buys no extra
    // diagnostic power and makes the suite flaky under parallel ctest.
    EXPECT_GT(s.mean, expected_ns * 0.95) << "a sleep cannot finish early";
    EXPECT_LT(s.mean, expected_ns * 5.0)
        << "a badly wrong frequency lands orders of magnitude out, not 5x";
    c.shutdown();
}

TEST(Tsc, TheCollectorPublishesItsCalibration) {
    Collector c;
    config    cfg   = quiet_config();
    cfg.point_count = static_cast<point_id>(pt::count);
    ASSERT_TRUE(c.start(cfg));
    c.flush();

    EXPECT_GT(c.tsc_hz(), 0.5e9);
    EXPECT_LT(c.tsc_hz(), 1.0e10);
    // Whatever the answer, it must be reported rather than assumed.
    SUCCEED() << "invariant: " << c.invariant_tsc();
    c.shutdown();
}

TEST(Tsc, CyclesOutputUnitSkipsConversionEntirely) {
    Collector c;
    config    cfg    = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.output_unit  = unit::cycles;
    ASSERT_TRUE(c.start(cfg));

    for (int i = 0; i < 100; ++i) {
        begin(pt::sleeper);
        end(pt::sleeper);
    }

    const stats s = c.snapshot_total(to_point(pt::sleeper));
    EXPECT_EQ(s.count, 100u);
#if SLICK_PERFMON_X86
    // Raw cycles for an empty span sit in the tens-to-hundreds range; if this
    // had been converted to nanoseconds it would be far smaller.
    EXPECT_GT(s.min, 1.0);
#else
    // Off x86 there is no conversion left to catch: the fallback clock counts
    // nanoseconds, so `cycles` and `nanoseconds` are the same number and this
    // property is the identity. The clock is also coarser than an empty span -
    // ~41.67 ns on Apple Silicon - so s.min is legitimately zero, whole spans
    // having landed inside one tick. Only the aggregate is still meaningful.
    EXPECT_GT(s.max, 0.0) << "100 spans cannot all have fitted inside one tick";
#endif
    c.shutdown();
}
