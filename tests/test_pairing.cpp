// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The pairing state machine is the core of the event model: the hot path emits
// unrelated stamps, and everything a user reads is reconstructed here. These
// tests drive the Pairer directly with chosen timestamps, so every path -
// including each anomaly - is provoked exactly rather than hoped for.

#include <slick/perfmon/pairing.hpp>

#include "test_support.hpp"

#include <gtest/gtest.h>

using namespace slick::perfmon;
using namespace slick::perfmon::test;

namespace {

constexpr point_id kPoint = 3;
constexpr uint8_t  kDecode = 1;
constexpr uint8_t  kMatch  = 2;
constexpr uint8_t  kSend   = 3;

Pairer make_pairer() {
    Pairer p;
    p.reset(quiet_config());
    return p;
}

}  // namespace

TEST(Pairing, SimpleSpanProducesOneTotal) {
    // begin and end need not be in the same scope, or even the same function;
    // to the backend they are just two events with the same key.
    Pairer p = make_pairer();
    p.on_event(ev(kPoint, kBeginStep, 1000));
    p.on_event(ev(kPoint, kEndStep, 1500));

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), 1u);
    EXPECT_EQ(total->interval.min(), 500u);

    // A span with no intermediate steps yields exactly one row. The begin->end
    // transition and the total would carry identical numbers, so only the
    // total is kept.
    EXPECT_EQ(stage_count(p), 1u);
    EXPECT_EQ(find_stage(p, kPoint, kBeginStep, kEndStep), nullptr);

    EXPECT_EQ(p.open_span_count(), 0u) << "a closed span must free its slot";
    EXPECT_EQ(p.anomalies(kPoint).orphan, 0u);
    EXPECT_EQ(p.anomalies(kPoint).abandoned, 0u);
}

TEST(Pairing, PipelineProducesOneStagePerTransitionPlusTheTotal) {
    Pairer p = make_pairer();
    p.on_event(ev(kPoint, kBeginStep, 1000));
    p.on_event(ev(kPoint, kDecode, 1100));   // 100
    p.on_event(ev(kPoint, kMatch, 1350));    // 250
    p.on_event(ev(kPoint, kSend, 1400));     // 50
    p.on_event(ev(kPoint, kEndStep, 1600));  // 200

    // Four transitions plus the synthetic total.
    EXPECT_EQ(stage_count(p), 5u);

    struct Expected {
        uint8_t  from;
        uint8_t  to;
        uint64_t cycles;
    };
    const Expected expected[] = {
        {kBeginStep, kDecode, 100},
        {kDecode, kMatch, 250},
        {kMatch, kSend, 50},
        {kSend, kEndStep, 200},
    };

    uint64_t sum = 0;
    for (const Expected& e : expected) {
        Stage* g = find_stage(p, kPoint, e.from, e.to);
        ASSERT_NE(g, nullptr) << "missing stage " << int(e.from) << "->" << int(e.to);
        EXPECT_EQ(g->interval.count(), 1u);
        EXPECT_EQ(g->interval.min(), e.cycles);
        sum += e.cycles;
    }

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.min(), 600u);
    EXPECT_EQ(sum, total->interval.min())
        << "the stages must account for the whole span, with nothing unexplained";
}

TEST(Pairing, ReBeginningAnOpenSpanCountsAbandonedAndStartsFresh) {
    Pairer p = make_pairer();
    p.on_event(ev(kPoint, kBeginStep, 1000));
    p.on_event(ev(kPoint, kBeginStep, 2000));  // the first span never ended
    p.on_event(ev(kPoint, kEndStep, 2300));

    EXPECT_EQ(p.anomalies(kPoint).abandoned, 1u);

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), 1u) << "only the second span completed";
    EXPECT_EQ(total->interval.min(), 300u)
        << "the total must measure from the second begin, not the first";
}

TEST(Pairing, StepWithNoOpenSpanCountsOrphan) {
    // This is what a dropped begin looks like from the collector's side, which
    // is why the README points at `dropped` first when orphan climbs.
    Pairer p = make_pairer();
    p.on_event(ev(kPoint, kDecode, 1000));
    p.on_event(ev(kPoint, kEndStep, 1100));

    EXPECT_EQ(p.anomalies(kPoint).orphan, 2u);
    EXPECT_EQ(stage_count(p), 0u) << "nothing may be recorded from an unpaired stamp";
    EXPECT_EQ(find_total(p, kPoint), nullptr);
}

TEST(Pairing, EndTwiceRecordsOnceAndCountsTheSecondAsOrphan) {
    Pairer p = make_pairer();
    p.on_event(ev(kPoint, kBeginStep, 1000));
    p.on_event(ev(kPoint, kEndStep, 1200));
    p.on_event(ev(kPoint, kEndStep, 1400));

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), 1u);
    EXPECT_EQ(p.anomalies(kPoint).orphan, 1u);
}

TEST(Pairing, StagesAreKeyedByTransitionNotByOrdering) {
    // Step numbers carry no ordering of their own - they name boundaries. So
    // 3 -> 1 is a perfectly legitimate stage, distinct from 1 -> 3.
    Pairer p = make_pairer();
    p.on_event(ev(kPoint, kBeginStep, 0));
    p.on_event(ev(kPoint, 3, 100));
    p.on_event(ev(kPoint, 1, 250));
    p.on_event(ev(kPoint, kEndStep, 300));

    Stage* three_to_one = find_stage(p, kPoint, 3, 1);
    ASSERT_NE(three_to_one, nullptr);
    EXPECT_EQ(three_to_one->interval.min(), 150u);

    EXPECT_EQ(find_stage(p, kPoint, 1, 3), nullptr)
        << "the reverse transition never happened and must not appear";
}

TEST(Pairing, BackwardsTimestampCountsInvalidAndDiscardsTheSpan) {
    // A thread that migrated across an unsynchronised TSC domain. Emitting one
    // bad stage plus a bad total from the same events would be worse than
    // emitting nothing, so the whole span goes.
    Pairer p = make_pairer();
    p.on_event(ev(kPoint, kBeginStep, 5000));
    p.on_event(ev(kPoint, kDecode, 4000));  // went backwards

    EXPECT_EQ(p.anomalies(kPoint).invalid, 1u);
    EXPECT_EQ(stage_count(p), 0u);
    EXPECT_EQ(p.open_span_count(), 0u);

    // And the discarded span must not haunt the next one.
    p.on_event(ev(kPoint, kEndStep, 6000));
    EXPECT_EQ(p.anomalies(kPoint).orphan, 1u);
}

TEST(Pairing, ZeroLengthStageIsRecordedRatherThanDropped) {
    Pairer p = make_pairer();
    p.on_event(ev(kPoint, kBeginStep, 1000));
    p.on_event(ev(kPoint, kEndStep, 1000));

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), 1u);
    EXPECT_EQ(total->interval.min(), 0u);
    EXPECT_EQ(p.anomalies(kPoint).invalid, 0u) << "equal timestamps are not backwards";
}

TEST(Pairing, DifferentPointsDoNotInterfere) {
    Pairer p = make_pairer();
    p.on_event(ev(1, kBeginStep, 100));
    p.on_event(ev(2, kBeginStep, 200));
    p.on_event(ev(1, kEndStep, 400));  // 300
    p.on_event(ev(2, kEndStep, 900));  // 700

    Stage* a = find_total(p, 1);
    Stage* b = find_total(p, 2);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->interval.min(), 300u);
    EXPECT_EQ(b->interval.min(), 700u);
    EXPECT_EQ(p.anomalies(1).abandoned, 0u);
    EXPECT_EQ(p.anomalies(2).abandoned, 0u);
}

TEST(Pairing, StageTableExhaustionCountsOutOfRange) {
    config cfg    = quiet_config();
    cfg.max_stages = 4;
    Pairer p;
    p.reset(cfg);

    // Each distinct point contributes at least its total, so twenty points is
    // well past a table of four.
    for (point_id i = 0; i < 20; ++i) {
        p.on_event(ev(i, kBeginStep, 1000));
        p.on_event(ev(i, kEndStep, 1100));
    }

    EXPECT_LE(stage_count(p), 4u) << "the table must not grow past its cap";

    uint64_t out_of_range = 0;
    for (point_id i = 0; i < 20; ++i) {
        out_of_range += p.anomalies(i).out_of_range;
    }
    EXPECT_GT(out_of_range, 0u) << "dropped transitions must be counted, not silently lost";
}

TEST(Pairing, ClearIntervalsStartsANewIntervalWithoutTouchingCumulative) {
    // The fold into cumulative belongs to report building, not to this call.
    // Doing it in both places would count every sample twice - which is
    // exactly the bug this test exists to prevent coming back.
    Pairer p = make_pairer();
    p.on_event(ev(kPoint, kBeginStep, 0));
    p.on_event(ev(kPoint, kEndStep, 100));

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), 1u);
    EXPECT_EQ(total->cumulative.count(), 0u);

    // What the collector does at flush time: fold, then clear.
    total->interval.merge_into(total->cumulative);
    p.clear_intervals();

    EXPECT_EQ(total->interval.count(), 0u) << "the interval restarts each flush";
    EXPECT_EQ(total->cumulative.count(), 1u) << "exactly one sample, not two";
    EXPECT_EQ(total->cumulative.min(), 100u);

    p.on_event(ev(kPoint, kBeginStep, 1000));
    p.on_event(ev(kPoint, kEndStep, 1300));
    total->interval.merge_into(total->cumulative);
    p.clear_intervals();

    EXPECT_EQ(total->cumulative.count(), 2u);
    EXPECT_EQ(total->cumulative.min(), 100u);
    EXPECT_EQ(total->cumulative.max(), 300u);
}

TEST(Pairing, OverheadIsSubtractedAndClampedAtZero) {
    Pairer p = make_pairer();
    p.set_overhead(40);

    p.on_event(ev(kPoint, kBeginStep, 0));
    p.on_event(ev(kPoint, kDecode, 100));    // 100 - 40 = 60
    p.on_event(ev(kPoint, kEndStep, 110));   // 10 - 40 -> clamped to 0

    Stage* first = find_stage(p, kPoint, kBeginStep, kDecode);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->interval.min(), 60u);

    Stage* second = find_stage(p, kPoint, kDecode, kEndStep);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->interval.min(), 0u) << "subtraction must never wrap below zero";
}

TEST(Pairing, TheTotalSubtractsOverheadOncePerTransitionSoTheStagesStillSumToIt) {
    // Regression: the total used to lose a single stamp's overhead however many
    // transitions it spanned, so a pipeline's stages and its total disagreed by
    // (n - 1) stamps. A multi-stage report whose parts do not add up to its
    // whole is worse than one with no overhead correction at all.
    Pairer p = make_pairer();
    constexpr uint64_t kOverhead = 10;
    p.set_overhead(kOverhead);

    p.on_event(ev(kPoint, kBeginStep, 1000));
    p.on_event(ev(kPoint, kDecode, 1100));   // 100 raw
    p.on_event(ev(kPoint, kMatch, 1300));    // 200 raw
    p.on_event(ev(kPoint, kEndStep, 1600));  // 300 raw; 600 raw end to end

    Stage* a     = find_stage(p, kPoint, kBeginStep, kDecode);
    Stage* b     = find_stage(p, kPoint, kDecode, kMatch);
    Stage* c     = find_stage(p, kPoint, kMatch, kEndStep);
    Stage* total = find_total(p, kPoint);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_NE(total, nullptr);

    EXPECT_EQ(a->interval.min(), 100u - kOverhead);
    EXPECT_EQ(b->interval.min(), 200u - kOverhead);
    EXPECT_EQ(c->interval.min(), 300u - kOverhead);

    EXPECT_EQ(total->interval.min(), 600u - 3u * kOverhead);
    EXPECT_EQ(total->interval.min(),
              a->interval.min() + b->interval.min() + c->interval.min())
        << "the stages have to account for the total, overhead correction included";
}

TEST(Pairing, ASimpleSpanStillLosesExactlyOneStampOfOverhead) {
    // The counterpart of the test above: one transition, one stamp subtracted.
    // Per-transition accounting must not over-correct the common case.
    Pairer p = make_pairer();
    p.set_overhead(25);

    p.on_event(ev(kPoint, kBeginStep, 0));
    p.on_event(ev(kPoint, kEndStep, 500));

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.min(), 475u);
}

TEST(Pairing, OverheadAccountingSurvivesARepeatedStep) {
    // Steps may revisit a value, so the transition count is not bounded by the
    // number of distinct steps - it has to be counted, not inferred.
    Pairer p = make_pairer();
    constexpr uint64_t kOverhead = 5;
    p.set_overhead(kOverhead);

    p.on_event(ev(kPoint, kBeginStep, 0));
    p.on_event(ev(kPoint, kDecode, 100));
    p.on_event(ev(kPoint, kMatch, 200));
    p.on_event(ev(kPoint, kDecode, 300));  // back to decode: a fourth transition
    p.on_event(ev(kPoint, kEndStep, 400));

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.min(), 400u - 4u * kOverhead);
}

TEST(Pairing, StageKeysDoNotCollide) {
    // total_key is (point << 16) with a zero step pair. A real transition
    // always ends on a step that is not kBeginStep, so the two cannot alias -
    // but that reasoning is worth pinning down.
    EXPECT_EQ(stage_key(kPoint, kBeginStep, kBeginStep), total_key(kPoint));
    for (uint32_t from = 0; from <= 0xFF; ++from) {
        for (uint32_t to = 1; to <= 0xFF; ++to) {
            EXPECT_NE(stage_key(kPoint, static_cast<uint8_t>(from), static_cast<uint8_t>(to)),
                      total_key(kPoint));
        }
    }
    EXPECT_NE(stage_key(1, 0, 1), stage_key(2, 0, 1));
}

TEST(Pairing, ResetClearsTheOverheadSoItMustBeSetAfterwards) {
    // The order is not a style choice. reset() clears the overhead along with
    // the tables, so a caller that sets it first gets silently raw timings -
    // which is exactly what Collector::run() did, making config's
    // subtract_overhead a no-op for every session.
    config cfg = quiet_config();
    Pairer p;

    p.reset(cfg);
    p.set_overhead(100);
    p.on_event(ev(kPoint, kBeginStep, 1000));
    p.on_event(ev(kPoint, kEndStep, 1500));
    ASSERT_NE(find_total(p, kPoint), nullptr);
    EXPECT_EQ(find_total(p, kPoint)->interval.min(), 400u)
        << "one transition, so one stamp's overhead comes off the total";

    p.set_overhead(100);
    p.reset(cfg);
    p.on_event(ev(kPoint, kBeginStep, 1000));
    p.on_event(ev(kPoint, kEndStep, 1500));
    ASSERT_NE(find_total(p, kPoint), nullptr);
    EXPECT_EQ(find_total(p, kPoint)->interval.min(), 500u)
        << "an overhead set before reset() is discarded, not remembered";
}

TEST(Pairing, ResetClearsSessionCounters) {
    // The peak and the overflow bucket are high-water marks, not gauges, so
    // nothing lowers them during a run. A Collector is restartable and reuses
    // its Pairer, and a mark left over from the previous session would be read
    // as something this session did.
    config cfg          = quiet_config();
    cfg.max_open_spans  = 4;
    cfg.max_stages      = 2;  // the per-point anomaly table is sized from this

    Pairer p;
    p.reset(cfg);

    for (span_seq s = 1; s <= 4; ++s) {
        p.on_event(ev(kPoint, kBeginStep, 1000 + s, s));
    }
    EXPECT_EQ(p.peak_open_spans(), 4u);

    // An end with no begin is an orphan and needs no stage row, so these three
    // points exist only in the anomaly table - which holds two.
    for (point_id q = 0; q < 3; ++q) {
        p.on_event(ev(q, kEndStep, 2000));
    }
    EXPECT_GT(p.overflow_anomalies().orphan, 0u) << "the third point must overflow";

    p.reset(cfg);
    EXPECT_EQ(p.peak_open_spans(), 0u);
    EXPECT_EQ(p.overflow_anomalies().orphan, 0u);
}
