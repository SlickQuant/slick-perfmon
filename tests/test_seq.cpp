// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The correlation id is what makes overlapping spans on one point measurable.
// Without it, two threads stamping the same point interleave as
// begin, begin, end, end and no backend can say which end belongs to which
// begin. These tests cover both that it works and that its limits are counted
// rather than silently producing a plausible wrong latency.

#include <slick/perfmon/pairing.hpp>

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace slick::perfmon;
using namespace slick::perfmon::test;

namespace {
constexpr point_id kPoint = 11;
constexpr uint8_t  kMid   = 5;
}  // namespace

TEST(Seq, InterleavedSpansPairByTheirCorrelationId) {
    Pairer p;
    p.reset(quiet_config());

    // Three spans opened before any of them closes - exactly the shape that
    // makes an unkeyed point ambiguous.
    p.on_event(ev(kPoint, kBeginStep, 1000, 1));
    p.on_event(ev(kPoint, kBeginStep, 1100, 2));
    p.on_event(ev(kPoint, kBeginStep, 1200, 3));
    EXPECT_EQ(p.open_span_count(), 3u);

    p.on_event(ev(kPoint, kEndStep, 1500, 2));  // 400
    p.on_event(ev(kPoint, kEndStep, 1900, 1));  // 900
    p.on_event(ev(kPoint, kEndStep, 2000, 3));  // 800

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), 3u);
    EXPECT_EQ(total->interval.min(), 400u);
    EXPECT_EQ(total->interval.max(), 900u);

    EXPECT_EQ(p.anomalies(kPoint).abandoned, 0u);
    EXPECT_EQ(p.anomalies(kPoint).orphan, 0u);
    EXPECT_EQ(p.open_span_count(), 0u);
}

TEST(Seq, IsNotPartOfTheStageKeySoEverySpanAggregatesIntoOneRow) {
    Pairer p;
    p.reset(quiet_config());

    for (span_seq q = 1; q <= 50; ++q) {
        p.on_event(ev(kPoint, kBeginStep, q * 1000, q));
        p.on_event(ev(kPoint, kMid, q * 1000 + 100, q));
        p.on_event(ev(kPoint, kEndStep, q * 1000 + 300, q));
    }

    // Two transitions plus the total - not 50 of each.
    EXPECT_EQ(stage_count(p), 3u);

    Stage* first = find_stage(p, kPoint, kBeginStep, kMid);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->interval.count(), 50u);

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), 50u);
    EXPECT_EQ(total->interval.min(), 300u);
}

TEST(Seq, UnkeyedSpansBehaveAsOneBucketPerPoint) {
    // kNoSeq is not a special case in the implementation, and this pins that
    // down: it is simply the single bucket a point gets when its spans never
    // overlap.
    Pairer p;
    p.reset(quiet_config());

    p.on_event(ev(kPoint, kBeginStep, 100));
    p.on_event(ev(kPoint, kEndStep, 200));
    p.on_event(ev(kPoint, kBeginStep, 300));
    p.on_event(ev(kPoint, kEndStep, 450));

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), 2u);
    EXPECT_EQ(total->interval.min(), 100u);
    EXPECT_EQ(total->interval.max(), 150u);
    EXPECT_EQ(p.anomalies(kPoint).abandoned, 0u);
}

TEST(Seq, SharingOneUnkeyedPointAcrossOverlappingSpansIsDetected) {
    // This is the documented failure mode. What matters is that it shows up as
    // `abandoned` rather than as a believable latency.
    Pairer p;
    p.reset(quiet_config());

    p.on_event(ev(kPoint, kBeginStep, 1000));  // "thread A"
    p.on_event(ev(kPoint, kBeginStep, 1100));  // "thread B" - clobbers A
    p.on_event(ev(kPoint, kEndStep, 1200));
    p.on_event(ev(kPoint, kEndStep, 1300));

    EXPECT_EQ(p.anomalies(kPoint).abandoned, 1u);
    EXPECT_EQ(p.anomalies(kPoint).orphan, 1u);

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), 1u) << "only one of the two spans can be resolved";
}

TEST(Seq, ReusingASeqWhileItsSpanIsOpenCountsAbandoned) {
    Pairer p;
    p.reset(quiet_config());

    p.on_event(ev(kPoint, kBeginStep, 1000, 7));
    p.on_event(ev(kPoint, kBeginStep, 1400, 7));  // same seq, still open
    p.on_event(ev(kPoint, kEndStep, 1600, 7));

    EXPECT_EQ(p.anomalies(kPoint).abandoned, 1u);

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), 1u);
    EXPECT_EQ(total->interval.min(), 200u) << "measured from the second begin";
}

TEST(Seq, DifferentPointsWithTheSameSeqAreDistinctSpans) {
    Pairer p;
    p.reset(quiet_config());

    p.on_event(ev(1, kBeginStep, 1000, 42));
    p.on_event(ev(2, kBeginStep, 1000, 42));
    p.on_event(ev(1, kEndStep, 1100, 42));
    p.on_event(ev(2, kEndStep, 1500, 42));

    ASSERT_NE(find_total(p, 1), nullptr);
    ASSERT_NE(find_total(p, 2), nullptr);
    EXPECT_EQ(find_total(p, 1)->interval.min(), 100u);
    EXPECT_EQ(find_total(p, 2)->interval.min(), 500u);
    EXPECT_EQ(p.anomalies(1).abandoned, 0u);
    EXPECT_EQ(p.anomalies(2).abandoned, 0u);
}

TEST(Seq, TruncatedSeqsThatAliasAreTreatedAsTheSameSpan) {
    // seq only has to be unique among the spans open on one point at one time.
    // Two values that differ above bit 24 alias, and that has to behave like
    // any other reuse - counted, not mispaired.
    Pairer p;
    p.reset(quiet_config());

    p.on_event(ev(kPoint, kBeginStep, 1000, 1));
    p.on_event(ev(kPoint, kBeginStep, 1100, kMaxSeq + 2));  // truncates to 1
    p.on_event(ev(kPoint, kEndStep, 1300, 1));

    EXPECT_EQ(p.anomalies(kPoint).abandoned, 1u);
    ASSERT_NE(find_total(p, kPoint), nullptr);
    EXPECT_EQ(find_total(p, kPoint)->interval.min(), 200u);
}

TEST(Seq, ExceedingMaxOpenSpansEvictsTheOldest) {
    config cfg          = quiet_config();
    cfg.max_open_spans  = 8;
    Pairer p;
    p.reset(cfg);

    // Open more spans than the table can hold. The oldest must be dropped and
    // counted, not leak and not push out a newer one.
    for (span_seq q = 1; q <= 12; ++q) {
        p.on_event(ev(kPoint, kBeginStep, 1000 + q, q));
    }
    EXPECT_LE(p.open_span_count(), 8u);
    EXPECT_EQ(p.anomalies(kPoint).abandoned, 4u);

    // The most recent span must have survived and must still pair.
    p.on_event(ev(kPoint, kEndStep, 2000, 12));
    ASSERT_NE(find_total(p, kPoint), nullptr);
    EXPECT_EQ(find_total(p, kPoint)->interval.count(), 1u);

    // The evicted one must now read as an orphan rather than pair with junk.
    p.on_event(ev(kPoint, kEndStep, 2100, 1));
    EXPECT_EQ(p.anomalies(kPoint).orphan, 1u);
}

TEST(Seq, SweepEvictsSpansThatAgeOut) {
    // A seq whose end never arrives - a crashed producer, an order never acked
    // - would otherwise hold its slot for the life of the process.
    Pairer p;
    p.reset(quiet_config());

    p.on_event(ev(kPoint, kBeginStep, 1000, 1));
    p.on_event(ev(kPoint, kBeginStep, 9000, 2));
    EXPECT_EQ(p.open_span_count(), 2u);

    // "now" is 10000; anything untouched for more than 5000 cycles goes.
    const size_t removed = p.sweep(10000, 5000);
    EXPECT_EQ(removed, 1u);
    EXPECT_EQ(p.open_span_count(), 1u);
    EXPECT_EQ(p.anomalies(kPoint).abandoned, 1u);

    // The young one survives and still pairs.
    p.on_event(ev(kPoint, kEndStep, 9500, 2));
    ASSERT_NE(find_total(p, kPoint), nullptr);
    EXPECT_EQ(find_total(p, kPoint)->interval.min(), 500u);
}

TEST(Seq, SweepWithNoAgeLimitDoesNothing) {
    Pairer p;
    p.reset(quiet_config());
    p.on_event(ev(kPoint, kBeginStep, 1000, 1));
    EXPECT_EQ(p.sweep(1'000'000, 0), 0u);
    EXPECT_EQ(p.open_span_count(), 1u);
}

TEST(Seq, IntermediateStepsKeepASpanAlive) {
    // seen_tsc has to advance on every event, not just on begin, or a long
    // multi-step span would be swept out from under itself.
    Pairer p;
    p.reset(quiet_config());

    p.on_event(ev(kPoint, kBeginStep, 1000, 1));
    p.on_event(ev(kPoint, kMid, 9500, 1));

    EXPECT_EQ(p.sweep(10000, 5000), 0u) << "the step refreshed the span";
    EXPECT_EQ(p.open_span_count(), 1u);
    EXPECT_EQ(p.anomalies(kPoint).abandoned, 0u);
}

TEST(Seq, PeakOpenSpansIsTracked) {
    Pairer p;
    p.reset(quiet_config());
    for (span_seq q = 1; q <= 20; ++q) {
        p.on_event(ev(kPoint, kBeginStep, 1000 + q, q));
    }
    EXPECT_EQ(p.peak_open_spans(), 20u);
    for (span_seq q = 1; q <= 20; ++q) {
        p.on_event(ev(kPoint, kEndStep, 2000 + q, q));
    }
    EXPECT_EQ(p.open_span_count(), 0u);
    EXPECT_EQ(p.peak_open_spans(), 20u) << "the peak is a high-water mark, not a gauge";
}

TEST(Seq, ManyConcurrentSpansAllResolve) {
    // The case that was impossible before seq: a large number of overlapping
    // spans on a single point, every one of them accounted for.
    constexpr span_seq kSpans = 2000;

    config cfg         = quiet_config();
    cfg.max_open_spans = 4096;
    Pairer p;
    p.reset(cfg);

    for (span_seq q = 1; q <= kSpans; ++q) {
        p.on_event(ev(kPoint, kBeginStep, q, q));
    }
    for (span_seq q = kSpans; q >= 1; --q) {  // close in reverse order
        p.on_event(ev(kPoint, kEndStep, 100000 + q, q));
    }

    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), kSpans);
    EXPECT_EQ(p.anomalies(kPoint).abandoned, 0u);
    EXPECT_EQ(p.anomalies(kPoint).orphan, 0u);
    EXPECT_EQ(p.open_span_count(), 0u);
}

TEST(Seq, EvictionStaysOldestFirstAcrossABatch) {
    // Finding the oldest span by scanning the table was O(table) on every
    // begin for as long as the overload lasted, so one scan now nominates a
    // batch of candidates and later evictions pop from it. The order that
    // buys the speed back must not change: candidates are still evicted
    // oldest first, because everything inserted since a scan is newer than
    // everything in it.
    config cfg         = quiet_config();
    cfg.max_open_spans = 64;  // a batch of eight, so the queue is really used
    Pairer p;
    p.reset(cfg);

    constexpr span_seq kCapacity = 64;
    for (span_seq q = 1; q <= kCapacity; ++q) {
        p.on_event(ev(kPoint, kBeginStep, 1000 + q, q));
    }
    ASSERT_EQ(p.open_span_count(), kCapacity);

    // Twelve more, which is more than one batch: the queue has to refill
    // mid-run and pick up exactly where it left off.
    constexpr span_seq kOverflow = 12;
    for (span_seq q = kCapacity + 1; q <= kCapacity + kOverflow; ++q) {
        p.on_event(ev(kPoint, kBeginStep, 1000 + q, q));
    }
    EXPECT_EQ(p.open_span_count(), kCapacity);
    EXPECT_EQ(p.anomalies(kPoint).abandoned, kOverflow);

    // Exactly seq 1..12 - the twelve oldest - must be the ones gone.
    for (span_seq q = 1; q <= kOverflow; ++q) {
        p.on_event(ev(kPoint, kEndStep, 5000, q));
    }
    EXPECT_EQ(p.anomalies(kPoint).orphan, kOverflow)
        << "the evicted spans must be the oldest, not an arbitrary batch";

    // And everything newer must still be open and still pair.
    Stage* total = find_total(p, kPoint);
    ASSERT_EQ(total, nullptr) << "no span has ended yet";
    for (span_seq q = kOverflow + 1; q <= kCapacity + kOverflow; ++q) {
        p.on_event(ev(kPoint, kEndStep, 6000, q));
    }
    total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->interval.count(), kCapacity);
    EXPECT_EQ(p.anomalies(kPoint).orphan, kOverflow) << "no newer span was lost";
}

TEST(Seq, ACandidateTouchedSinceTheScanIsNotEvicted) {
    // A nominated candidate is only still the oldest span if nothing has
    // touched it since the scan. Evicting one that has been would report a
    // live, active span as abandoned - and abandoned is a counter that tells
    // the reader a latency may be wrong.
    config cfg         = quiet_config();
    cfg.max_open_spans = 32;  // a batch of four
    Pairer p;
    p.reset(cfg);

    constexpr span_seq kCapacity = 32;
    for (span_seq q = 1; q <= kCapacity; ++q) {
        p.on_event(ev(kPoint, kBeginStep, 1000 + q, q));
    }

    // Fills the queue with seq 1..4 and evicts seq 1, the oldest.
    p.on_event(ev(kPoint, kBeginStep, 2000, kCapacity + 1));
    EXPECT_EQ(p.anomalies(kPoint).abandoned, 1u);

    // Seq 2 is next in the queue - but a step makes it the most recently
    // touched span in the table, so it is no longer anybody's oldest.
    p.on_event(ev(kPoint, kMid, 3000, 2));

    p.on_event(ev(kPoint, kBeginStep, 4000, kCapacity + 2));
    EXPECT_EQ(p.anomalies(kPoint).abandoned, 2u);

    // Seq 2 must have survived and must still pair from its own begin.
    p.on_event(ev(kPoint, kEndStep, 5000, 2));
    Stage* total = find_total(p, kPoint);
    ASSERT_NE(total, nullptr) << "the touched span was evicted instead of skipped";
    EXPECT_EQ(total->interval.count(), 1u);
    EXPECT_EQ(total->interval.min(), 5000u - 1002u);

    // Seq 3 - the genuinely oldest once seq 2 was skipped - is the one gone.
    p.on_event(ev(kPoint, kEndStep, 6000, 3));
    EXPECT_EQ(p.anomalies(kPoint).orphan, 1u);
}
