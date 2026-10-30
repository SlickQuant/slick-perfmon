// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// detail::thread_ring: the per-thread SPSC ring. Loss accounting has to be
// exact (consumed + lost == published) and a reader racing a lapping producer
// must never be handed a torn or stale sample.

#include <slick/perfmon/detail/thread_ring.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace slick::perfmon;
using namespace slick::perfmon::detail;

namespace {

/// Heap storage for one ring, laid out as the ring table will lay it out.
struct test_ring {
    explicit test_ring(uint64_t capacity) : slots(capacity), scratch(capacity) {
        view.head = &head;
        view.data = slots.data();
        view.mask = capacity - 1;
    }
    alignas(64) std::atomic<uint64_t> head{0};
    std::vector<sample> slots;
    std::vector<sample> scratch;
    ring_view           view;
};

/// Fields derived from the index, so any sample can be checked on its own.
uint32_t point_for(uint64_t i) { return static_cast<uint32_t>(i * 2654435761u); }
uint32_t event_for(uint64_t i) { return static_cast<uint32_t>(i ^ 0xA5A5A5A5u); }

void publish_n(ring_writer& w, uint64_t from, uint64_t n) {
    for (uint64_t i = from; i < from + n; ++i) {
        w.publish(i, point_for(i), event_for(i));
    }
}

/// Read everything there is, checking each sample against its index.
uint64_t drain(test_ring& r, uint64_t& cursor, uint64_t& lost, size_t max_n = 1u << 20) {
    uint64_t consumed = 0;
    for (;;) {
        const uint64_t before = cursor;
        const ring_batch b    = ring_read(r.view, cursor, r.scratch.data(),
                                          max_n < r.scratch.size() ? max_n : r.scratch.size(), lost);
        if (b.size == 0) {
            if (cursor == before) {
                return consumed;
            }
            continue;
        }
        for (size_t k = 0; k < b.size; ++k) {
            EXPECT_EQ(b.data[k].timestamp, cursor + k);
            EXPECT_EQ(b.data[k].point, point_for(cursor + k));
            EXPECT_EQ(b.data[k].event, event_for(cursor + k));
        }
        cursor += b.size;
        consumed += b.size;
    }
}

}  // namespace

TEST(ThreadRing, RoundTrip) {
    test_ring   r(16);
    ring_writer w;
    w.attach(r.view);
    publish_n(w, 0, 10);

    uint64_t cursor = 0, lost = 0;
    EXPECT_EQ(drain(r, cursor, lost), 10u);
    EXPECT_EQ(cursor, 10u);
    EXPECT_EQ(lost, 0u);
}

TEST(ThreadRing, WrapsAroundTheEnd) {
    test_ring   r(8);
    ring_writer w;
    w.attach(r.view);
    uint64_t cursor = 0, lost = 0;

    publish_n(w, 0, 5);
    EXPECT_EQ(drain(r, cursor, lost), 5u);
    publish_n(w, 5, 6);  // slots 5..7, then 0..2
    EXPECT_EQ(drain(r, cursor, lost), 6u);
    EXPECT_EQ(lost, 0u);
}

TEST(ThreadRing, ReadsInChunksOfMaxN) {
    test_ring   r(16);
    ring_writer w;
    w.attach(r.view);
    publish_n(w, 0, 10);

    uint64_t   cursor = 0, lost = 0;
    ring_batch b      = ring_read(r.view, cursor, r.scratch.data(), 3, lost);
    ASSERT_EQ(b.size, 3u);
    EXPECT_EQ(b.data[0].timestamp, 0u);
    cursor += b.size;
    EXPECT_EQ(drain(r, cursor, lost, 4), 7u);
    EXPECT_EQ(lost, 0u);
}

TEST(ThreadRing, EmptyRingReadsNothing) {
    test_ring  r(8);
    uint64_t   cursor = 0, lost = 0;
    ring_batch b      = ring_read(r.view, cursor, r.scratch.data(), 8, lost);
    EXPECT_EQ(b.size, 0u);
    EXPECT_EQ(cursor, 0u);
    EXPECT_EQ(lost, 0u);
}

TEST(ThreadRing, KeepsTheNewestCapacityMinusOne) {
    // The slot of index head - capacity may be mid-write, so it never counts as
    // intact, even with the producer idle.
    test_ring   r(8);
    ring_writer w;
    w.attach(r.view);
    uint64_t cursor = 0, lost = 0;

    publish_n(w, 0, 7);
    EXPECT_EQ(drain(r, cursor, lost), 7u);
    EXPECT_EQ(lost, 0u);

    publish_n(w, 7, 8);
    EXPECT_EQ(drain(r, cursor, lost), 7u);
    EXPECT_EQ(lost, 1u);
}

TEST(ThreadRing, FarBehindCountsEveryOverwrittenSample) {
    test_ring   r(8);
    ring_writer w;
    w.attach(r.view);
    publish_n(w, 0, 29);

    uint64_t cursor = 0, lost = 0;
    EXPECT_EQ(drain(r, cursor, lost), 7u);
    EXPECT_EQ(lost, 22u);
    EXPECT_EQ(cursor, 29u);
}

TEST(ThreadRing, NewOwnerContinuesFromTheHead) {
    // A ring outlives the thread that claimed it. The next owner picks up the
    // head where it was, so the collector's cursor stays valid across owners.
    test_ring r(16);
    {
        ring_writer a;
        a.attach(r.view);
        publish_n(a, 0, 3);
    }
    ring_writer b;
    b.attach(r.view);
    EXPECT_EQ(b.next, 3u);
    publish_n(b, 3, 2);

    uint64_t cursor = 0, lost = 0;
    EXPECT_EQ(drain(r, cursor, lost), 5u);
    EXPECT_EQ(lost, 0u);
}

TEST(ThreadRing, LappingProducerNeverYieldsATornSample) {
    // A tiny ring and a producer flat out, so the reader is lapped constantly.
    // drain() checks every delivered sample against its index; a torn or stale
    // one would carry another index's fields. Under ThreadSanitizer this is
    // also the data-race check.
    constexpr uint64_t kTotal = 2'000'000;
    test_ring          r(64);
    std::atomic<bool>  done{false};

    std::jthread producer([&] {
        ring_writer w;
        w.attach(r.view);
        publish_n(w, 0, kTotal);
        done.store(true, std::memory_order_release);
    });

    uint64_t cursor = 0, lost = 0, consumed = 0;
    while (!done.load(std::memory_order_acquire)) {
        consumed += drain(r, cursor, lost, 16);
    }
    consumed += drain(r, cursor, lost, 16);

    EXPECT_EQ(consumed + lost, kTotal);
    EXPECT_GT(consumed, 0u);
}
