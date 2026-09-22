// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The RAII guard is a wrapper over two stamps, so these tests check that it
// emits exactly the stamps it claims to - no more on a move, no fewer on a
// throw - by counting completed spans through a live collector.

#include <slick/perfmon.hpp>
#include <slick/perfmon/collector.hpp>

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <utility>
#include <vector>

using namespace slick::perfmon;
using namespace slick::perfmon::test;

namespace {

enum class pt : point_id { alpha = 0, beta = 1, gamma = 2, count };
constexpr uint8_t kMid = 4;

class ScopedTest : public ::testing::Test {
protected:
    void SetUp() override {
        config cfg      = quiet_config();
        cfg.point_count = static_cast<point_id>(pt::count);
        ASSERT_TRUE(collector_.start(cfg));
    }

    void TearDown() override { collector_.shutdown(); }

    uint64_t completed(pt p) { return collector_.snapshot_total(to_point(p)).count; }

    stats total(pt p) { return collector_.snapshot_total(to_point(p)); }

    Collector collector_;

private:
    // The guard talks to whatever this module's hot path points at, which
    // start() sets, so a locally-owned Collector is enough - no need to touch
    // the process-wide instance.
};

}  // namespace

TEST_F(ScopedTest, RecordsExactlyOneSpanOnNormalExit) {
    {
        ScopedSample guard(pt::alpha);
        (void)guard;
    }
    EXPECT_EQ(completed(pt::alpha), 1u);
    EXPECT_EQ(total(pt::alpha).abandoned, 0u);
    EXPECT_EQ(total(pt::alpha).orphan, 0u);
}

TEST_F(ScopedTest, RecordsOnAnEarlyReturn) {
    auto fn = [](bool bail) {
        ScopedSample guard(pt::alpha);
        if (bail) {
            return;
        }
        (void)guard;
    };
    fn(true);
    fn(false);
    EXPECT_EQ(completed(pt::alpha), 2u);
}

TEST_F(ScopedTest, RecordsDuringExceptionUnwinding) {
    // Usually what you want, but it means a throwing region shows up as an
    // outlier rather than as nothing - which is why cancel() exists.
    try {
        ScopedSample guard(pt::alpha);
        (void)guard;
        throw std::runtime_error("boom");
    } catch (const std::runtime_error&) {
    }
    EXPECT_EQ(completed(pt::alpha), 1u);
}

TEST_F(ScopedTest, CancelSuppressesTheEndStamp) {
    {
        ScopedSample guard(pt::alpha);
        guard.cancel();
    }
    EXPECT_EQ(completed(pt::alpha), 0u) << "a cancelled span must record nothing";

    // The span stays open, so the next begin on the same key closes it as
    // abandoned - that is the documented consequence of cancelling.
    {
        ScopedSample guard(pt::alpha);
        (void)guard;
    }
    EXPECT_EQ(completed(pt::alpha), 1u);
    EXPECT_EQ(total(pt::alpha).abandoned, 1u);
}

TEST_F(ScopedTest, AMovedFromGuardStampsNothing) {
    {
        ScopedSample first(pt::alpha);
        ScopedSample second(std::move(first));
        (void)second;
    }
    // Exactly one span, not two: the moved-from guard must be disarmed.
    EXPECT_EQ(completed(pt::alpha), 1u);
    EXPECT_EQ(total(pt::alpha).orphan, 0u);
}

TEST_F(ScopedTest, MoveAssignmentClosesTheGuardBeingOverwritten) {
    {
        ScopedSample a(pt::alpha);
        ScopedSample b(pt::beta);
        a = std::move(b);  // a's span ends here, b is disarmed
    }
    EXPECT_EQ(completed(pt::alpha), 1u);
    EXPECT_EQ(completed(pt::beta), 1u);
}

TEST_F(ScopedTest, StepLandsInsideTheSpan) {
    {
        ScopedSample guard(pt::alpha);
        guard.step(kMid);
    }
    collector_.flush();

    const stats first = collector_.snapshot(to_point(pt::alpha), kBeginStep, kMid);
    const stats last  = collector_.snapshot(to_point(pt::alpha), kMid, kEndStep);
    EXPECT_EQ(first.count, 1u);
    EXPECT_EQ(last.count, 1u);
    EXPECT_EQ(completed(pt::alpha), 1u);
}

TEST_F(ScopedTest, CarriesItsSeqToEveryStamp) {
    // Overlapping guards on one point are exactly what seq is for.
    {
        ScopedSample a(pt::alpha, 100u);
        ScopedSample b(pt::alpha, 200u);
        ScopedSample c(pt::alpha, 300u);
        a.step(kMid);
        b.step(kMid);
        c.step(kMid);
    }
    EXPECT_EQ(completed(pt::alpha), 3u);
    EXPECT_EQ(total(pt::alpha).abandoned, 0u)
        << "distinct seqs must keep three overlapping guards apart";
}

TEST_F(ScopedTest, TwoScopeMacrosCoexistInOneFunction) {
    // If the __LINE__ suffixing were broken this would not compile at all.
    {
        SLICK_PERFMON_SCOPE(pt::alpha);
        SLICK_PERFMON_SCOPE(pt::beta);
    }
    EXPECT_EQ(completed(pt::alpha), 1u);
    EXPECT_EQ(completed(pt::beta), 1u);
}

TEST_F(ScopedTest, TheMacroAcceptsASeq) {
    {
        SLICK_PERFMON_SCOPE(pt::gamma, 77u);
    }
    EXPECT_EQ(completed(pt::gamma), 1u);
}

TEST_F(ScopedTest, NestingDifferentPointsRecordsBoth) {
    {
        SLICK_PERFMON_SCOPE(pt::alpha);
        {
            SLICK_PERFMON_SCOPE(pt::beta);
        }
    }
    EXPECT_EQ(completed(pt::alpha), 1u);
    EXPECT_EQ(completed(pt::beta), 1u);

    // True nesting: the inner span is contained by the outer one.
    EXPECT_LE(total(pt::beta).mean, total(pt::alpha).mean);
}

TEST_F(ScopedTest, NestingTheSamePointAndSeqIsTheAbandonedCase) {
    {
        SLICK_PERFMON_SCOPE(pt::alpha);
        {
            SLICK_PERFMON_SCOPE(pt::alpha);  // same key - re-opens
        }
    }
    EXPECT_EQ(total(pt::alpha).abandoned, 1u);
}

TEST_F(ScopedTest, NestingTheSamePointWithDistinctSeqsWorks) {
    {
        SLICK_PERFMON_SCOPE(pt::alpha, 1u);
        {
            SLICK_PERFMON_SCOPE(pt::alpha, 2u);
        }
    }
    EXPECT_EQ(completed(pt::alpha), 2u);
    EXPECT_EQ(total(pt::alpha).abandoned, 0u);
}

TEST_F(ScopedTest, ExposesItsIdentity) {
    ScopedSample guard(pt::gamma, 55u);
    EXPECT_EQ(guard.point(), to_point(pt::gamma));
    EXPECT_EQ(guard.seq(), 55u);
    guard.cancel();
}
