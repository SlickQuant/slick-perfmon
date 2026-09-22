// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// Compiled with SLICK_PERFMON_ENABLED=0. The claim being tested is that a
// production build can keep every instrumentation call in the source and pay
// nothing for it - so this file is deliberately its own executable.

#include <slick/perfmon.hpp>
#include <slick/perfmon/collector.hpp>

#include <gtest/gtest.h>

#include <sstream>
#include <type_traits>

static_assert(SLICK_PERFMON_ENABLED == 0,
              "this translation unit only proves anything with the library switched off");

using namespace slick::perfmon;

namespace {

enum class pt : point_id { alpha = 0, beta = 1, count };
constexpr uint8_t kMid = 6;

/// Incremented by anything the instrumentation evaluates. It must never move.
int g_side_effects = 0;

point_id noisy_point() {
    ++g_side_effects;
    return to_point(pt::alpha);
}

uint8_t noisy_step() {
    ++g_side_effects;
    return kMid;
}

}  // namespace

TEST(Disabled, EveryEntryPointStillCompiles) {
    // The whole point: instrumented source needs no #ifdef of its own.
    begin(pt::alpha);
    step(pt::alpha, kMid);
    end(pt::alpha);
    begin(pt::beta, 42u);
    end(pt::beta, 42u);
    stamp<true>(0, make_event(kBeginStep));
    attach(nullptr);
    SUCCEED();
}

TEST(Disabled, TheMacrosExpandToNothing) {
    SLICK_PERFMON_BEGIN(pt::alpha);
    SLICK_PERFMON_STEP(pt::alpha, kMid);
    SLICK_PERFMON_END(pt::alpha);
    {
        SLICK_PERFMON_SCOPE(pt::beta);
    }
    {
        SLICK_PERFMON_SCOPE(pt::beta, 7u);
    }
    SUCCEED();
}

TEST(Disabled, MacroArgumentsAreNeverEvaluated) {
    g_side_effects = 0;
    SLICK_PERFMON_BEGIN(noisy_point());
    SLICK_PERFMON_STEP(noisy_point(), noisy_step());
    SLICK_PERFMON_END(noisy_point());
    {
        SLICK_PERFMON_SCOPE(noisy_point());
    }
    EXPECT_EQ(g_side_effects, 0)
        << "a disabled macro that still evaluates its arguments is not free";
}

TEST(Disabled, TheScopeMacroLeavesNoObjectBehind) {
    // ((void)0) is a statement, so two in one scope must still compile, and
    // neither may declare anything.
    {
        SLICK_PERFMON_SCOPE(pt::alpha);
        SLICK_PERFMON_SCOPE(pt::beta);
    }
    SUCCEED();
}

TEST(Disabled, TheCollectorApiIsPresentButInert) {
    // Setup code in main() compiles unchanged; it simply does nothing.
    config cfg;
    cfg.point_count = static_cast<point_id>(pt::count);
    cfg.path        = "should_never_be_written.csv";

    Collector& c = Collector::instance();
    EXPECT_FALSE(c.start(cfg));
    EXPECT_FALSE(c.running());
    EXPECT_FALSE(c.enabled());
    c.set_enabled(true);
    EXPECT_FALSE(c.enabled()) << "there is nothing to enable";
    c.flush();

    EXPECT_EQ(c.snapshot_total(0).count, 0u);
    EXPECT_TRUE(c.reports().empty());
    EXPECT_DOUBLE_EQ(c.tsc_hz(), 0.0);
    EXPECT_EQ(c.overhead_cycles(), 0u);

    std::ostringstream os;
    c.dump_summary(os);
    EXPECT_TRUE(os.str().empty()) << "no collector, no summary";

    c.shutdown();
}

TEST(Disabled, TypesAreStillAvailableForUserCode) {
    // A user's perf_points.hpp refers to these, so they must survive even when
    // nothing is measured.
    static_assert(sizeof(sample) == 16);
    static_assert(step_of(make_event(kEndStep, 5)) == kEndStep);
    static_assert(seq_of(make_event(kEndStep, 5)) == 5u);
    EXPECT_EQ(to_point(pt::beta), 1u);
}

TEST(Disabled, ScopedSampleIsTriviallyCheap) {
    // It still exists as a type - user code may name it - but its constructor
    // and destructor call the no-op stamps, so the optimizer removes it whole.
    static_assert(std::is_nothrow_destructible_v<ScopedSample>);
    ScopedSample guard(pt::alpha, 3u);
    EXPECT_EQ(guard.point(), 0u);
    EXPECT_EQ(guard.seq(), 3u);
    guard.step(kMid);
    guard.cancel();
    SUCCEED();
}
