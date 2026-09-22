// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#include <slick/perfmon.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>

using namespace slick::perfmon;

// The packing is the contract between the hot path and the backend, so most of
// it is checked at compile time: a regression here should not survive to a
// test run.
static_assert(make_event(kBeginStep) == 0u);
static_assert(step_of(make_event(kBeginStep)) == kBeginStep);
static_assert(step_of(make_event(kEndStep)) == kEndStep);
static_assert(seq_of(make_event(kEndStep)) == kNoSeq);
static_assert(step_of(make_event(0x42, 0x1234u)) == 0x42);
static_assert(seq_of(make_event(0x42, 0x1234u)) == 0x1234u);

// A seq wider than 24 bits must lose its top bits and nothing else - in
// particular it must never bleed into the step byte.
static_assert(step_of(make_event(kEndStep, 0xFFFFFFFFu)) == kEndStep);
static_assert(seq_of(make_event(kEndStep, 0xFFFFFFFFu)) == kMaxSeq);

static_assert(sizeof(sample) == 16);
static_assert(alignof(sample) == 16);
static_assert(std::is_trivially_copyable_v<sample>);

TEST(EventId, RoundTripsEveryStep) {
    for (uint32_t s = 0; s <= 0xFFu; ++s) {
        const event_id e = make_event(static_cast<uint8_t>(s), 0xABCDEFu);
        EXPECT_EQ(step_of(e), static_cast<uint8_t>(s));
        EXPECT_EQ(seq_of(e), 0xABCDEFu);
    }
}

TEST(EventId, RoundTripsSeqAcrossItsRange) {
    for (span_seq q = 0; q < kMaxSeq; q = q * 3 + 1) {
        const event_id e = make_event(kEndStep, q);
        EXPECT_EQ(seq_of(e), q);
        EXPECT_EQ(step_of(e), kEndStep);
    }
    EXPECT_EQ(seq_of(make_event(kEndStep, kMaxSeq)), kMaxSeq);
}

TEST(EventId, SeqTruncatesWithoutCorruptingTheStep) {
    // 24 bits of seq plus a step byte fills the 32-bit field exactly; anything
    // above kMaxSeq has to be dropped rather than shifted into the step.
    const event_id e = make_event(0x7F, kMaxSeq + 1);
    EXPECT_EQ(step_of(e), 0x7F);
    EXPECT_EQ(seq_of(e), 0u);
}

TEST(EventId, BeginAndEndAreDistinctFromEveryIntermediateStep) {
    for (uint32_t s = 1; s < 0xFFu; ++s) {
        EXPECT_NE(static_cast<uint8_t>(s), kBeginStep);
        EXPECT_NE(static_cast<uint8_t>(s), kEndStep);
    }
}

TEST(EventId, PointAndEventAreAdjacentAndTheRecordStartsAtTheTimestamp) {
    // Not cosmetic. The layout is the cross-process contract for a shared ring,
    // and keeping the three fields packed into 16 aligned bytes is what lets a
    // snapshot of a slot be one cache line and never two.
    //
    // It used to also let the compiler fuse `point` and `event` into a single
    // 8-byte immediate store. It no longer can: those are relaxed atomic stores
    // now, because a lapping producer overwrites a slot the collector is
    // reading and plain stores made that undefined. The benchmark puts the lost
    // merge inside the run-to-run noise. See sample::store().
    EXPECT_EQ(offsetof(sample, point) + sizeof(point_id), offsetof(sample, event));
    EXPECT_EQ(offsetof(sample, timestamp), 0u);
}

namespace {
enum class scoped_point : point_id { alpha = 7, beta = 9 };
}

TEST(EventId, AcceptsAScopedEnumWithoutACast) {
    EXPECT_EQ(to_point(scoped_point::alpha), 7u);
    EXPECT_EQ(to_point(scoped_point::beta), 9u);
    EXPECT_EQ(to_point(42), 42u);

    // The whole point of the point_like concept: these must compile as written.
    begin(scoped_point::alpha);
    step(scoped_point::alpha, 3);
    end(scoped_point::alpha);
    ScopedSample guard(scoped_point::beta, 11u);
    EXPECT_EQ(guard.point(), 9u);
    EXPECT_EQ(guard.seq(), 11u);
}
