// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// Per-thread rings through a live collector: claiming, falling back to the
// shared ring, handing rings back on thread exit, and merging the rings by
// timestamp so spans that cross threads still pair.

#include <slick/perfmon.hpp>
#include <slick/perfmon/collector.hpp>

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace slick::perfmon;
using namespace slick::perfmon::test;

namespace {

constexpr point_id kSpan  = 1;
constexpr point_id kValue = 2;

size_t rings_in_state(const Collector& c, uint64_t state) {
    const detail::ring_table& t = c.session()->rings;
    size_t                    n = 0;
    for (uint32_t i = 0; i < t.count(); ++i) {
        if (detail::ring_state(t.slot(i).owner.load(std::memory_order_acquire)) == state) {
            ++n;
        }
    }
    return n;
}

template <typename Pred>
bool eventually(Pred&& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// From here on this module counts as another one to @p c's session, the way
/// a DLL stamping into the host's session does. `creator` is a plain field, so
/// only while no other thread is stamping.
void as_if_made_elsewhere(const Collector& c) {
    static const int elsewhere = 0;
    const_cast<detail::session*>(c.session())->creator = &elsewhere;
}

config ring_config(uint32_t rings) {
    config cfg           = quiet_config();
    cfg.point_count      = 3;
    cfg.max_thread_rings = rings;
    return cfg;
}

void expect_clean(const stats& s, uint64_t count) {
    EXPECT_EQ(s.count, count);
    EXPECT_EQ(s.dropped, 0u);
    EXPECT_EQ(s.orphan, 0u);
    EXPECT_EQ(s.abandoned, 0u);
    EXPECT_EQ(s.invalid, 0u);
}

/// begin on @p opener, end on @p closer, one span at a time, @p n times.
void cross_thread_spans(int n, bool reverse = false) {
    std::atomic<int> opened{0};
    std::atomic<int> closed{0};
    auto opener = [&] {
        for (int i = 1; i <= n; ++i) {
            begin(kSpan);
            opened.store(i, std::memory_order_release);
            while (closed.load(std::memory_order_acquire) != i) {
            }
        }
    };
    auto closer = [&] {
        for (int i = 1; i <= n; ++i) {
            while (opened.load(std::memory_order_acquire) != i) {
            }
            end(kSpan);
            closed.store(i, std::memory_order_release);
        }
    };
    // Started first, so it binds first: with one ring left, it gets it.
    std::thread first = reverse ? std::thread(closer) : std::thread(opener);
    std::thread second = reverse ? std::thread(opener) : std::thread(closer);
    first.join();
    second.join();
}

}  // namespace

TEST(ThreadRings, EachThreadClaimsItsOwnRing) {
    Collector c;
    ASSERT_TRUE(c.start(ring_config(8)));
    EXPECT_EQ(rings_in_state(c, detail::kRingActive), 1u) << "start() binds its own thread";

    constexpr int    kThreads = 4;
    constexpr int    kSpans   = 2000;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([&, t] {
            begin(kSpan, static_cast<span_seq>(t + 1));
            end(kSpan, static_cast<span_seq>(t + 1));
            ready.fetch_add(1);
            while (!go.load()) {
            }
            for (int i = 1; i < kSpans; ++i) {
                begin(kSpan, static_cast<span_seq>(t + 1));
                end(kSpan, static_cast<span_seq>(t + 1));
            }
        });
    }
    while (ready.load() < kThreads) {
    }
    EXPECT_EQ(rings_in_state(c, detail::kRingActive), 1u + kThreads);
    go.store(true);
    for (auto& th : pool) {
        th.join();
    }

    expect_clean(c.snapshot_total(kSpan), kThreads * kSpans);
    // Exited threads hand their rings back; drained, the collector frees them.
    EXPECT_TRUE(eventually([&] { return rings_in_state(c, detail::kRingFree) == 7u; }));
    c.shutdown();
}

TEST(ThreadRings, AFreedRingIsClaimedAgain) {
    Collector c;
    ASSERT_TRUE(c.start(ring_config(2)));  // start() takes one for this thread

    for (int round = 0; round < 5; ++round) {
        std::thread([] {
            for (int i = 0; i < 100; ++i) {
                begin(kSpan);
                end(kSpan);
            }
        }).join();
        ASSERT_TRUE(eventually([&] { return rings_in_state(c, detail::kRingFree) == 1u; }));
    }
    expect_clean(c.snapshot_total(kSpan), 500u);
    EXPECT_EQ(c.ring()->initial_reading_index(), 0u) << "nothing should have fallen back";
    c.shutdown();
}

TEST(ThreadRings, ThreadsPastTheCapUseTheSharedRing) {
    Collector c;
    ASSERT_TRUE(c.start(ring_config(2)));

    constexpr int            kThreads = 4;
    constexpr int            kSpans   = 1000;
    std::atomic<int>         ready{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([&, t] {
            const span_seq seq = static_cast<span_seq>(t + 1);
            begin(kSpan, seq);
            end(kSpan, seq);
            ready.fetch_add(1);
            while (ready.load() < kThreads) {  // all bound before any exits
            }
            for (int i = 1; i < kSpans; ++i) {
                begin(kSpan, seq);
                end(kSpan, seq);
            }
        });
    }
    for (auto& th : pool) {
        th.join();
    }
    expect_clean(c.snapshot_total(kSpan), kThreads * kSpans);
    EXPECT_GT(c.ring()->initial_reading_index(), 0u) << "three threads had no ring";
    c.shutdown();
}

TEST(ThreadRings, RingsOffSendsEveryThreadToTheSharedRing) {
    Collector c;
    ASSERT_TRUE(c.start(ring_config(0)));
    EXPECT_EQ(c.session()->rings.count(), 0u);

    for (int i = 0; i < 1000; ++i) {
        begin(kSpan);
        end(kSpan);
    }
    expect_clean(c.snapshot_total(kSpan), 1000u);
    EXPECT_EQ(c.ring()->initial_reading_index(), 2000u);
    c.shutdown();
}

TEST(ThreadRings, ASpanCrossingThreadsPairs) {
    // The begin and the end sit in different rings. Draining the rings one
    // after the other would see some ends before their begins; the merge by
    // timestamp must not.
    Collector c;
    ASSERT_TRUE(c.start(ring_config(8)));
    cross_thread_spans(20000);
    expect_clean(c.snapshot_total(kSpan), 20000u);
    c.shutdown();
}

TEST(ThreadRings, ASpanCrossingIntoTheSharedRingPairs) {
    // One thread on a ring, the other past the cap on the shared ring.
    Collector c;
    ASSERT_TRUE(c.start(ring_config(2)));
    cross_thread_spans(10000);
    cross_thread_spans(10000, true);
    expect_clean(c.snapshot_total(kSpan), 20000u);
    c.shutdown();
}

TEST(ThreadRings, ValuesAndSpansShareARing) {
    Collector c;
    ASSERT_TRUE(c.start(ring_config(4)));
    std::thread([] {
        for (uint64_t i = 0; i < 5000; ++i) {
            begin(kSpan);
            value(kValue, i);
            end(kSpan);
        }
    }).join();
    expect_clean(c.snapshot_total(kSpan), 5000u);
    const stats v = c.snapshot_value(kValue);
    EXPECT_EQ(v.count, 5000u);
    EXPECT_EQ(v.max, 4999.0);
    c.shutdown();
}

TEST(ThreadRings, OverwrittenSamplesAreCountedExactly) {
    // A tiny ring the collector cannot keep up with: whatever it does not
    // pair it must count, so the two add up to what was published.
    Collector c;
    config    cfg            = ring_config(4);
    cfg.thread_ring_capacity = 64;
    cfg.poll_interval        = std::chrono::milliseconds(20);
    ASSERT_TRUE(c.start(cfg));

    constexpr uint64_t kValues = 100000;
    std::thread([] {
        for (uint64_t i = 0; i < kValues; ++i) {
            value(kValue, i);
        }
    }).join();
    const stats v = c.snapshot_value(kValue);
    EXPECT_GT(v.dropped, 0u);
    EXPECT_EQ(v.count + v.dropped, kValues);
    c.shutdown();
}

TEST(ThreadRings, ARestartRebindsTheThread) {
    // The second session may land where the first one was freed; the binding
    // must notice it is a different session regardless.
    for (int round = 0; round < 3; ++round) {
        Collector c;
        ASSERT_TRUE(c.start(ring_config(4)));
        for (int i = 0; i < 100; ++i) {
            begin(kSpan);
            end(kSpan);
        }
        expect_clean(c.snapshot_total(kSpan), 100u);
        c.shutdown();
        ASSERT_TRUE(c.start(ring_config(4)));
        for (int i = 0; i < 50; ++i) {
            begin(kSpan);
            end(kSpan);
        }
        expect_clean(c.snapshot_total(kSpan), 50u);
        c.shutdown();
    }
}

TEST(ThreadRings, ALocalRestartTakesBackItsThreadRings) {
    // Regression: every restart allocated a fresh table and retired the old
    // one until the Collector died - 64 MiB per cycle at the defaults. The
    // table of the same geometry is now taken back, heads and all.
    Collector c;
    ASSERT_TRUE(c.start(ring_config(4)));
    const sample* first = c.session()->rings.view(0).data;
    for (int round = 0; round < 3; ++round) {
        c.shutdown();
        ASSERT_TRUE(c.start(ring_config(4)));
        EXPECT_EQ(c.session()->rings.view(0).data, first) << "round " << round;
        for (int i = 0; i < 100; ++i) {
            begin(kSpan);
            end(kSpan);
        }
        expect_clean(c.snapshot_total(kSpan), 100u);
    }

    // Another geometry gets its own table; returning takes the first one back.
    c.shutdown();
    ASSERT_TRUE(c.start(ring_config(8)));
    EXPECT_NE(c.session()->rings.view(0).data, first);
    c.shutdown();
    ASSERT_TRUE(c.start(ring_config(4)));
    EXPECT_EQ(c.session()->rings.view(0).data, first);
    c.shutdown();
}

TEST(ThreadRings, AnOversizedRingTableIsRefusedByName) {
    // 4096 rings of 65536 slots is 4 GiB: named here rather than a bad_alloc.
    Collector c;
    config    cfg            = ring_config(4096);
    cfg.thread_ring_capacity = 1u << 16;
    EXPECT_THROW((void)c.start(cfg), std::invalid_argument);
    cfg                      = ring_config(64);
    cfg.thread_ring_capacity = 1u << 26;
    EXPECT_THROW((void)c.start(cfg), std::invalid_argument);
    cfg.thread_ring_capacity = 1u << 16;
    EXPECT_TRUE(c.start(cfg));
    c.shutdown();
}

TEST(ThreadRings, FlushIncludesAStampFromACoreRunningAhead) {
    // A stamp read on a core whose TSC runs ahead of the collector's must still
    // make the flush asked for after it. 3000 cycles is more skew than a
    // synchronised TSC shows, and inside the flush's margin at any clock rate.
    Collector c;
    ASSERT_TRUE(c.start(ring_config(4)));
    const detail::session& s     = *c.session();
    const uint64_t         ahead = detail::rdtsc_begin() + 3000;
    detail::publish(detail::bound_writer(s), s, ahead, kSpan, make_event(kBeginStep));
    detail::publish(detail::bound_writer(s), s, ahead + 100, kSpan, make_event(kEndStep));
    expect_clean(c.snapshot_total(kSpan), 1u);
    c.shutdown();
}

TEST(ThreadRings, ASnapshotReturnsWhileAThreadKeepsRecordingValues) {
    // Regression: a flush read each ring until it came up empty. A value costs
    // its producer less than it costs the collector, so a thread recording
    // values never let it come up empty and snapshot() never returned.
    Collector c;
    ASSERT_TRUE(c.start(ring_config(4)));
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    uint64_t          published = 0;
    std::thread       t([&] {
        running.store(true);
        while (!stop.load(std::memory_order_relaxed)) {
            value(kValue, 1);
            ++published;
        }
    });
    while (!running.load()) {
    }
    for (int i = 0; i < 5; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        (void)c.snapshot_value(kValue);
        EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(2)) << i;
    }
    stop.store(true);
    t.join();
    const stats v = c.snapshot_value(kValue);
    EXPECT_EQ(v.count + v.dropped, published);
    c.shutdown();
}

TEST(ThreadRings, ASnapshotDuringCrossThreadSpansStaysExact) {
    // A flush pairs stamps younger than the hold-back, so it must still see a
    // span's begin before its end when the two land in different rings while
    // the flush is reading them. Rings big enough that a slow (Debug) collector
    // flushing back to back loses nothing.
    Collector c;
    config    cfg            = ring_config(8);
    cfg.thread_ring_capacity = 1u << 18;
    ASSERT_TRUE(c.start(cfg));
    constexpr int     kSpans = 100000;
    std::atomic<bool> done{false};
    std::thread       spans([&] {
        cross_thread_spans(kSpans);
        done.store(true);
    });
    while (!done.load()) {
        (void)c.snapshot_total(kSpan);
    }
    spans.join();
    expect_clean(c.snapshot_total(kSpan), kSpans);
    c.shutdown();
}

TEST(ThreadRings, StampsThatTieAcrossRingsPairCleanly) {
    // Regression: on a coarse clock a begin and its end on another thread can
    // share a tick, and the merge broke the tie by heap position, pairing the
    // end first: an orphan, then an abandoned span. Two spans, every stamp on
    // one tick. Heap order on a tie is the library's choice, so both layouts:
    // one of them takes the end first unless the tie is handled.
    for (const bool ends_low : {true, false}) {
        SCOPED_TRACE(ends_low ? "ends in the lower ring" : "ends in the higher ring");
        Collector c;
        ASSERT_TRUE(c.start(ring_config(8)));
        detail::ring_writer closer;
        detail::ring_writer opener;
        closer.attach(c.session()->rings.view(ends_low ? 6 : 7));
        opener.attach(c.session()->rings.view(ends_low ? 7 : 6));

        const uint64_t t = detail::rdtsc_step();
        opener.publish(t, kSpan, make_event(kBeginStep));
        closer.publish(t, kSpan, make_event(kEndStep));
        opener.publish(t, kSpan, make_event(kBeginStep));
        closer.publish(t, kSpan, make_event(kEndStep));

        expect_clean(c.snapshot_total(kSpan), 2);
        c.shutdown();
    }
}

TEST(ThreadRings, StampsThatTieAcrossThreeRingsPairCleanly) {
    // Regression: the tie check looked only at the next source in the heap.
    // Two ends tied with a third ring holding both their begins, so it found
    // the other end and paired one as an orphan. The begins' ring takes each
    // position in turn: whatever order the heap leaves tied sources in, one
    // layout puts both ends ahead of it.
    for (const uint32_t opens : {5u, 6u, 7u}) {
        SCOPED_TRACE(opens);
        Collector c;
        ASSERT_TRUE(c.start(ring_config(8)));
        detail::ring_writer opener;
        detail::ring_writer closer1;
        detail::ring_writer closer2;
        opener.attach(c.session()->rings.view(opens));
        closer1.attach(c.session()->rings.view(opens == 5u ? 6u : 5u));
        closer2.attach(c.session()->rings.view(opens == 7u ? 6u : 7u));

        const uint64_t t = detail::rdtsc_step();
        opener.publish(t, kSpan, make_event(kBeginStep, 1));
        opener.publish(t, kSpan, make_event(kBeginStep, 2));
        closer1.publish(t, kSpan, make_event(kEndStep, 1));
        closer2.publish(t, kSpan, make_event(kEndStep, 2));

        expect_clean(c.snapshot_total(kSpan), 2);
        c.shutdown();
    }
}

TEST(ThreadRings, ATableAnotherModuleAttachedIsNotReused) {
    // Regression: a local restart took the previous table back, but a DLL that
    // attach()ed to the previous session and did not attach again keeps its
    // own thread-local writer into that table, and went on writing into rings
    // the new session had handed out: their heads went backwards and spans
    // were lost, uncounted. Modelled here by a copy of this thread's writer.
    Collector c;
    ASSERT_TRUE(c.start(ring_config(4)));
    as_if_made_elsewhere(c);
    attach(c.session());  // as the DLL does
    const sample* first = c.session()->rings.view(0).data;
    for (int i = 0; i < 16; ++i) {
        begin(kSpan);
        end(kSpan);
    }
    detail::ring_writer dll = detail::tls_state.ring;
    ASSERT_NE(dll.data, nullptr);

    c.shutdown();
    ASSERT_TRUE(c.start(ring_config(4)));
    EXPECT_NE(c.session()->rings.view(0).data, first);
    for (uint64_t i = 0; i < 1000; ++i) {
        begin(kSpan);
        dll.publish(i, kValue, make_event(kValueStep));
        end(kSpan);
    }
    expect_clean(c.snapshot_total(kSpan), 1000u);
    EXPECT_EQ(c.snapshot_value(kValue).count, 0u) << "the stale module's values reached this session";
    c.shutdown();
}

TEST(ThreadRings, AModuleStampingWithoutAttachHoldsItsTableBackToo) {
    // Regression: only attach() marked a session as written by another module,
    // so a module stamping through Collector::session() without it still had
    // the table reused under its writers. Binding a thread marks it now.
    Collector c;
    ASSERT_TRUE(c.start(ring_config(4)));
    const detail::session& s1 = *c.session();
    as_if_made_elsewhere(c);
    const sample*       first = s1.rings.view(0).data;
    detail::ring_writer dll;
    std::thread([&] {
        for (int i = 0; i < 16; ++i) {
            detail::publish(detail::bound_writer(s1), s1, detail::rdtsc_begin(), kSpan,
                            make_event(kBeginStep));
            detail::publish(detail::bound_writer(s1), s1, detail::rdtsc_step(), kSpan,
                            make_event(kEndStep));
        }
        dll = detail::tls_state.ring;
    }).join();
    ASSERT_NE(dll.data, nullptr);

    c.shutdown();
    ASSERT_TRUE(c.start(ring_config(4)));
    EXPECT_NE(c.session()->rings.view(0).data, first);
    for (uint64_t i = 0; i < 1000; ++i) {
        begin(kSpan);
        dll.publish(i, kValue, make_event(kValueStep));
        end(kSpan);
    }
    expect_clean(c.snapshot_total(kSpan), 1000u);
    EXPECT_EQ(c.snapshot_value(kValue).count, 0u) << "the stale module's values reached this session";
    c.shutdown();
}

TEST(ThreadRings, ATableComesBackOnceTheOtherModuleHasMovedOn) {
    // A DLL that attaches again after every restart, as it must, has left the
    // previous session's table by the restart after that: the host cycles two
    // tables rather than adding one per restart.
    Collector                  c;
    std::vector<const sample*> tables;
    for (int round = 0; round < 4; ++round) {
        ASSERT_TRUE(c.start(ring_config(4)));
        as_if_made_elsewhere(c);
        attach(c.session());
        tables.push_back(c.session()->rings.view(0).data);
        c.shutdown();
    }
    EXPECT_NE(tables[1], tables[0]);
    EXPECT_EQ(tables[2], tables[0]);
    EXPECT_EQ(tables[3], tables[1]);
}

TEST(ThreadRings, AThreadExitingWhileDisabledHandsItsRingBack) {
    Collector c;
    ASSERT_TRUE(c.start(ring_config(4)));

    std::atomic<bool> stamped{false};
    std::atomic<bool> may_exit{false};
    std::thread       t([&] {
        begin(kSpan);
        end(kSpan);
        stamped.store(true);
        while (!may_exit.load()) {
        }
    });
    while (!stamped.load()) {
    }
    EXPECT_EQ(rings_in_state(c, detail::kRingActive), 2u);
    c.set_enabled(false);
    may_exit.store(true);
    t.join();
    EXPECT_TRUE(eventually([&] { return rings_in_state(c, detail::kRingFree) == 3u; }));
    c.set_enabled(true);
    c.shutdown();
}

TEST(ThreadRings, PrepareThreadClaimsBeforeTheFirstStamp) {
    Collector c;
    ASSERT_TRUE(c.start(ring_config(4)));
    std::thread([&] {
        prepare_thread();
        EXPECT_EQ(rings_in_state(c, detail::kRingActive), 2u);
    }).join();
    c.shutdown();
}
