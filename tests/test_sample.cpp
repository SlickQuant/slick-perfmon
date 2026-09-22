// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The concurrent-access contract of `sample`.
//
// The ring overwrites rather than blocking, so a producer that laps the
// collector writes a slot the collector is reading - by design, and counted as
// `dropped`. What that must *not* be is a data race in the C++ sense, which is
// undefined behaviour rather than a merely inaccurate number. sample::store()
// and sample::load() are what make the overlap defined, and these tests are
// what keep a later "simplification" back to plain assignment from passing.
//
// Under ThreadSanitizer the concurrency test below is the real assertion: the
// EXPECTs hold either way, but the sanitizer fails the run if the accesses stop
// being atomic.

#include <slick/perfmon.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>
#include <type_traits>

using namespace slick::perfmon;

TEST(Sample, StoreAndLoadRoundTrip) {
    sample s{};
    s.store(0x0123456789ABCDEFull, 42u, make_event(kEndStep, 0x5A5A5Au));

    const sample got = s.load();
    EXPECT_EQ(got.timestamp, 0x0123456789ABCDEFull);
    EXPECT_EQ(got.point, 42u);
    EXPECT_EQ(step_of(got.event), kEndStep);
    EXPECT_EQ(seq_of(got.event), 0x5A5A5Au);
}

TEST(Sample, StoreWritesTheSameBytesPlainAssignmentWould) {
    // The layout is a cross-process contract: a shared-memory ring is written by
    // one build and read by another. Relaxed atomics were chosen partly because
    // they change how the fields are written and not what ends up in them - if
    // that ever stopped being true, every attached peer would misread the ring.
    sample atomic_written{};
    atomic_written.store(0xDEADBEEFCAFEF00Dull, 7u, make_event(3u, 0x00ABCDu));

    sample plain{};
    plain.timestamp = 0xDEADBEEFCAFEF00Dull;
    plain.point     = 7u;
    plain.event     = make_event(3u, 0x00ABCDu);

    EXPECT_EQ(std::memcmp(&atomic_written, &plain, sizeof(sample)), 0);
}

TEST(Sample, LoadReturnsACopyNotAView) {
    // The bug this guards: the collector used to hand the pairer a reference
    // into live ring memory, and the pairer reads `event`, `point` and
    // `timestamp` several times each. A producer lapping between two of those
    // reads could pair a step from one generation against a timestamp from
    // another. A snapshot cannot drift, whatever the slot does afterwards.
    sample slot{};
    slot.store(100u, 1u, make_event(kBeginStep));

    sample snap = slot.load();
    slot.store(999u, 2u, make_event(kEndStep));

    EXPECT_EQ(snap.timestamp, 100u);
    EXPECT_EQ(snap.point, 1u);
    EXPECT_EQ(step_of(snap.event), kBeginStep);
    static_assert(std::is_same_v<decltype(slot.load()), sample>,
                  "load() must return by value or the snapshot is not one");
}


namespace {

/// Joins however the test body leaves.
///
/// A gtest fatal assertion returns from the test function, and destroying a
/// joinable std::thread that way calls std::terminate - so one failed check
/// would abort the run instead of reporting which check failed. The loops below
/// tally violations rather than asserting in place, but the guard is what makes
/// that a style choice instead of the only thing standing between a future
/// ASSERT_ and a terminate.
struct join_guard {
    std::thread& t;
    ~join_guard() {
        if (t.joinable()) {
            t.join();
        }
    }
};

/// Spin until `flag` is set, so two threads are demonstrably running at once.
void wait_for(const std::atomic<bool>& flag) {
    while (!flag.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

}  // namespace

TEST(Sample, ConcurrentStoreAndLoadIsDefined) {
    // One writer overwriting a single slot while a reader snapshots it: the
    // overlap a lapped ring produces, reduced to its smallest form. Before
    // store()/load() this was a plain read/write race and ThreadSanitizer
    // failed the run on it.
    //
    // The two threads have to be live at the same instant or the sanitizer has
    // no overlap to observe and this passes for the wrong reason - a writer
    // that runs to completion before the reader's first load proves nothing.
    // So the writer waits for the reader to reach its loop, and then keeps
    // writing until the reader has taken a meaningful number of snapshots.
    //
    // The fields can still mix generations - relaxed atomics make the overlap
    // defined, not ordered, and mixing is exactly what `dropped` warns about -
    // so the check is per field: every value observed must be one that was
    // actually written, never a torn half of one.
    constexpr uint64_t kGenerations = 1000000;  // stays under kMaxSeq, so seq round-trips
    constexpr uint64_t kMinWrites   = 50000;
    constexpr uint64_t kMinReads    = 50000;
    // Bounds the writer if the reader leaves early. A broken test should come
    // back red, not hang the CI job on a thread nobody will ever release.
    constexpr uint64_t kWriteCap = 100000000;

    sample slot{};
    slot.store(1u, 1u, make_event(kBeginStep, 1u));

    std::atomic<bool>     reader_running{false};
    std::atomic<bool>     stop{false};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> writes{0};

    std::thread writer([&] {
        wait_for(reader_running);
        uint64_t i = 0;
        while (++i < kWriteCap) {
            // Every field carries the same generation, so a value that was
            // never written is detectable whichever field it lands in.
            const uint64_t gen = 1u + (i % kGenerations);
            slot.store(gen, static_cast<point_id>(gen),
                       make_event(kBeginStep, static_cast<span_seq>(gen)));
            if (i >= kMinWrites && reads.load(std::memory_order_relaxed) >= kMinReads) {
                break;
            }
        }
        writes.store(i, std::memory_order_relaxed);
        stop.store(true, std::memory_order_release);
    });
    const join_guard joiner{writer};

    // Tallied rather than asserted in place: gtest bookkeeping inside the loop
    // would slow the reader enough to change what it races against, and a fatal
    // assertion here would return past the join.
    uint64_t taken = 0;
    uint64_t bad   = 0;
    reader_running.store(true, std::memory_order_release);
    while (!stop.load(std::memory_order_acquire)) {
        const sample got = slot.load();
        if (got.timestamp < 1u || got.timestamp > kGenerations ||
            got.point < 1u || static_cast<uint64_t>(got.point) > kGenerations ||
            step_of(got.event) != kBeginStep ||
            seq_of(got.event) < 1u || static_cast<uint64_t>(seq_of(got.event)) > kGenerations) {
            ++bad;
        }
        reads.store(++taken, std::memory_order_relaxed);
    }
    writer.join();

    EXPECT_EQ(bad, 0u) << "a snapshot held a value that was never stored";
    EXPECT_GE(taken, kMinReads);
    EXPECT_GE(writes.load(std::memory_order_relaxed), kMinWrites);
    EXPECT_LT(writes.load(std::memory_order_relaxed), kWriteCap)
        << "the writer hit its cap, so the reader never got going and nothing overlapped";
}

TEST(Sample, StampingIntoAnOverwritingRingDoesNotRaceUndefined) {
    // The same overlap at the level the library actually runs it: a ring far too
    // small for the traffic, so the producer laps the reader continuously. The
    // reader drains the way Collector::drain_once() does - snapshot first, use
    // the copy - and every sample it gets back has to be a plausible one.
    //
    // Same handshake as above, and for the same reason: a producer that retires
    // before the reader starts would leave a full ring to drain in peace, which
    // is the one situation this test is not about.
#if SLICK_PERFMON_ENABLED
    constexpr uint32_t kCapacity   = 16;  // laps almost immediately
    constexpr uint64_t kMinStamps  = 200000;
    constexpr uint64_t kMinDrained = 10000;
    constexpr uint64_t kStampCap   = 100000000;

    sample_queue ring(kCapacity);

    std::atomic<bool>     reader_running{false};
    std::atomic<bool>     stop{false};
    std::atomic<uint64_t> drained{0};
    std::atomic<uint64_t> stamped{0};

    std::thread producer([&] {
        wait_for(reader_running);
        uint64_t i = 0;
        while (++i < kStampCap) {
            detail::stamp_into<true>(ring, 5u, make_event(kBeginStep, 9u));
            if (i >= kMinStamps && drained.load(std::memory_order_relaxed) >= kMinDrained) {
                break;
            }
        }
        stamped.store(i, std::memory_order_relaxed);
        stop.store(true, std::memory_order_release);
    });
    const join_guard joiner{producer};

    uint64_t cursor = 0;
    uint64_t seen   = 0;
    uint64_t bad    = 0;
    reader_running.store(true, std::memory_order_release);
    while (!stop.load(std::memory_order_acquire)) {
        auto [ptr, n] = ring.read(cursor);
        if (ptr == nullptr || n == 0) {
            continue;
        }
        for (uint32_t i = 0; i < n; ++i) {
            // Every stamp carries the same point and event, so even a snapshot
            // caught mid-overwrite must come back with these exact values. The
            // check cannot flake on a mixed generation, only on a torn field.
            const sample s = ptr[i].load();
            if (s.point != 5u || step_of(s.event) != kBeginStep || seq_of(s.event) != 9u) {
                ++bad;
            }
        }
        seen += n;
        drained.store(seen, std::memory_order_relaxed);
    }
    producer.join();

    EXPECT_EQ(bad, 0u) << "a snapshot held a value that was never stamped";
    EXPECT_GE(seen, kMinDrained);
    EXPECT_LT(stamped.load(std::memory_order_relaxed), kStampCap)
        << "the producer hit its cap, so the reader never got going";
    EXPECT_GT(ring.loss_count(), 0u) << "a 16-slot ring cannot have kept up";
#else
    GTEST_SKIP() << "no ring to overwrite when instrumentation is compiled out";
#endif
}
