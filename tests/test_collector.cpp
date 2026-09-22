// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// End-to-end through a live collector in local mode: real stamps, a real ring,
// a real background thread.

#include <slick/perfmon.hpp>
#include <slick/perfmon/collector.hpp>

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace slick::perfmon;
using namespace slick::perfmon::test;

namespace {

enum class pt : point_id { work = 0, other = 1, count };
constexpr uint8_t kMid = 3;

}  // namespace

TEST(CollectorTest, SingleThreadedSpansAreAllAccountedFor) {
    Collector c;
    config    cfg   = quiet_config();
    cfg.point_count = static_cast<point_id>(pt::count);
    ASSERT_TRUE(c.start(cfg));

    constexpr int kSpans = 10000;
    for (int i = 0; i < kSpans; ++i) {
        begin(pt::work);
        end(pt::work);
    }

    const stats s = c.snapshot_total(to_point(pt::work));
    EXPECT_EQ(s.count, kSpans);
    EXPECT_EQ(s.dropped, 0u);
    EXPECT_EQ(s.abandoned, 0u);
    EXPECT_EQ(s.orphan, 0u);
    EXPECT_EQ(s.invalid, 0u);
    EXPECT_GT(s.mean, 0.0);
    c.shutdown();
}

TEST(CollectorTest, MultiThreadedWithOnePointPerThread) {
    Collector c;
    config    cfg      = quiet_config();
    cfg.point_count    = 8;
    cfg.queue_capacity = 1u << 18;
    ASSERT_TRUE(c.start(cfg));

    // Deliberately sized so the whole run fits in the ring: 8 x 2000 spans is
    // 32k stamps against 262k slots. Pairing costs more per event than stamping
    // does, so several hot producers can outrun one collector - asserting zero
    // drops only means something when nothing *could* have been overwritten
    // even if the collector never woke up. Sustained overload has its own test.
    constexpr int kThreads = 8;
    constexpr int kSpans   = 2000;
    static_assert(kThreads * kSpans * 2 < (1 << 18), "the run must fit in the ring");

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kSpans; ++i) {
                begin(static_cast<point_id>(t));
                end(static_cast<point_id>(t));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    for (int t = 0; t < kThreads; ++t) {
        const stats s = c.snapshot_total(static_cast<point_id>(t));
        EXPECT_EQ(s.count, kSpans) << "point " << t;
        EXPECT_EQ(s.dropped, 0u) << "point " << t;
        EXPECT_EQ(s.abandoned, 0u) << "point " << t;
        EXPECT_EQ(s.orphan, 0u) << "point " << t;
    }
    c.shutdown();
}

TEST(CollectorTest, MultiThreadedSharingOnePointKeyedBySeq) {
    // The case that is impossible without a correlation id: every thread on the
    // same point, told apart by seq alone.
    Collector c;
    config    cfg      = quiet_config();
    cfg.point_count    = static_cast<point_id>(pt::count);
    cfg.queue_capacity = 1u << 18;
    cfg.max_open_spans = 4096;
    ASSERT_TRUE(c.start(cfg));

    constexpr int kThreads = 8;
    constexpr int kSpans   = 2000;
    static_assert(kThreads * kSpans * 2 < (1 << 18), "the run must fit in the ring");

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            // Spans never overlap within a thread, so the thread index alone is
            // a sufficient discriminator.
            const span_seq seq = static_cast<span_seq>(t + 1);
            for (int i = 0; i < kSpans; ++i) {
                begin(pt::work, seq);
                end(pt::work, seq);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    const stats s = c.snapshot_total(to_point(pt::work));
    EXPECT_EQ(s.count, static_cast<uint64_t>(kThreads) * kSpans);
    EXPECT_EQ(s.dropped, 0u);
    EXPECT_EQ(s.abandoned, 0u) << "distinct seqs must not collide";
    EXPECT_EQ(s.orphan, 0u);
    c.shutdown();
}

TEST(CollectorTest, MultiStepSpansSumToTheirTotal) {
    Collector c;
    config    cfg   = quiet_config();
    cfg.point_count = static_cast<point_id>(pt::count);
    ASSERT_TRUE(c.start(cfg));

    constexpr int kSpans = 5000;
    for (int i = 0; i < kSpans; ++i) {
        begin(pt::work);
        step(pt::work, kMid);
        end(pt::work);
    }
    // All three rows must come from ONE snapshot. Each snapshot() call flushes,
    // and every flush refines the TSC calibration slightly, so three separate
    // calls would convert the same cycle counts with three marginally different
    // frequencies - enough to break an exact sum.
    stats first{}, last{}, total{};
    for (const StageReport& r : c.reports()) {
        if (r.point != to_point(pt::work)) {
            continue;
        }
        if (r.from == kBeginStep && r.to == kMid) {
            first = r.cumulative;
        } else if (r.from == kMid && r.to == kEndStep) {
            last = r.cumulative;
        } else if (r.from == kBeginStep && r.to == kBeginStep) {
            total = r.cumulative;
        }
    }

    EXPECT_EQ(first.count, kSpans);
    EXPECT_EQ(last.count, kSpans);
    EXPECT_EQ(total.count, kSpans);
    // The means are averages over the same spans, so the parts have to account
    // for the whole with nothing unexplained.
    EXPECT_NEAR(first.mean + last.mean, total.mean, total.mean * 1e-9);
    c.shutdown();
}

TEST(CollectorTest, AnUndersizedRingReportsDropsRatherThanHidingThem) {
    // The ring overwrites rather than blocking, so a producer that outruns the
    // collector loses samples. What matters is that it is counted.
    Collector c;
    config    cfg      = quiet_config();
    cfg.point_count    = static_cast<point_id>(pt::count);
    cfg.queue_capacity = 64;                             // deliberately tiny
    cfg.poll_interval  = std::chrono::milliseconds(50);  // and slow to drain
    ASSERT_TRUE(c.start(cfg));

    for (int i = 0; i < 200000; ++i) {
        begin(pt::work);
        end(pt::work);
    }

    const stats s = c.snapshot_total(to_point(pt::work));
    EXPECT_GT(s.dropped, 0u) << "a 64-slot ring cannot have kept up";
    EXPECT_LT(s.count, 200000u) << "lost stamps must show up as missing spans";

    // Deliberately no assertion on orphan/abandoned here. The ring overwrites
    // whole contiguous runs, so most losses take a begin and its end together
    // and leave no trace beyond the count - only a skip that lands between the
    // two produces an anomaly. `dropped` is the reliable signal that data was
    // lost; the anomaly counters are what tells you a *surviving* span may
    // have been mispaired.
    c.shutdown();
}

TEST(CollectorTest, SustainedMultiProducerLoadCanOutrunOneCollector) {
    // A characteristic worth pinning down rather than discovering in
    // production: pairing an event costs more than stamping one, so several
    // threads stamping flat out will eventually outpace a single collector and
    // the ring will overwrite. The contract is not "never drops" - it is
    // "never drops silently".
    Collector c;
    config    cfg      = quiet_config();
    cfg.point_count    = 8;
    cfg.queue_capacity = 1u << 12;  // small, to reach the condition quickly
    ASSERT_TRUE(c.start(cfg));

    constexpr int kThreads = 8;
    constexpr int kSpans   = 50000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kSpans; ++i) {
                begin(static_cast<point_id>(t));
                end(static_cast<point_id>(t));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    uint64_t dropped = 0;
    uint64_t counted = 0;
    for (int t = 0; t < kThreads; ++t) {
        const stats s = c.snapshot_total(static_cast<point_id>(t));
        dropped = std::max(dropped, s.dropped);  // ring-wide, same on every row
        counted += s.count;
    }

    EXPECT_GT(dropped, 0u) << "this load is meant to overflow a 4096-slot ring";
    EXPECT_LT(counted, static_cast<uint64_t>(kThreads) * kSpans);
    c.shutdown();
}

TEST(CollectorTest, RuntimeDisableStopsRecordingAndReEnableResumes) {
    Collector c;
    config    cfg   = quiet_config();
    cfg.point_count = static_cast<point_id>(pt::count);
    ASSERT_TRUE(c.start(cfg));

    for (int i = 0; i < 100; ++i) {
        begin(pt::work);
        end(pt::work);
    }
    EXPECT_EQ(c.snapshot_total(to_point(pt::work)).count, 100u);
    EXPECT_TRUE(c.enabled());

    c.set_enabled(false);
    EXPECT_FALSE(c.enabled());
    for (int i = 0; i < 500; ++i) {
        begin(pt::work);
        end(pt::work);
    }
    EXPECT_EQ(c.snapshot_total(to_point(pt::work)).count, 100u)
        << "nothing may be recorded while disabled";

    c.set_enabled(true);
    for (int i = 0; i < 50; ++i) {
        begin(pt::work);
        end(pt::work);
    }
    EXPECT_EQ(c.snapshot_total(to_point(pt::work)).count, 150u);
    c.shutdown();
}

TEST(CollectorTest, StampsBeforeStartAndAfterShutdownAreIgnored) {
    // An instrumented binary whose collector is not running must cost nothing
    // and record nothing, rather than crash or buffer forever.
    begin(pt::work);
    end(pt::work);

    Collector c;
    config    cfg   = quiet_config();
    cfg.point_count = static_cast<point_id>(pt::count);
    ASSERT_TRUE(c.start(cfg));
    begin(pt::work);
    end(pt::work);
    EXPECT_EQ(c.snapshot_total(to_point(pt::work)).count, 1u);
    c.shutdown();

    begin(pt::work);
    end(pt::work);
    SUCCEED() << "stamping after shutdown must be harmless";
}

TEST(CollectorTest, RestartingClearsTheStatistics) {
    Collector c;
    config    cfg   = quiet_config();
    cfg.point_count = static_cast<point_id>(pt::count);

    ASSERT_TRUE(c.start(cfg));
    for (int i = 0; i < 100; ++i) {
        begin(pt::work);
        end(pt::work);
    }
    EXPECT_EQ(c.snapshot_total(to_point(pt::work)).count, 100u);

    ASSERT_TRUE(c.start(cfg)) << "start() must be restartable";
    EXPECT_EQ(c.snapshot_total(to_point(pt::work)).count, 0u);
    for (int i = 0; i < 7; ++i) {
        begin(pt::work);
        end(pt::work);
    }
    EXPECT_EQ(c.snapshot_total(to_point(pt::work)).count, 7u);
    c.shutdown();
}

TEST(CollectorTest, ShutdownDrainsWhatWasAlreadyPublished) {
    Collector c;
    config    cfg   = quiet_config();
    cfg.point_count = static_cast<point_id>(pt::count);
    // Long enough that the periodic flush cannot be what drains these.
    cfg.flush_interval = std::chrono::seconds(60);
    ASSERT_TRUE(c.start(cfg));

    for (int i = 0; i < 1000; ++i) {
        begin(pt::work);
        end(pt::work);
    }
    c.shutdown();

    // reports() after shutdown reads the final flush the collector performed on
    // its way out.
    bool found = false;
    for (const StageReport& r : c.reports()) {
        if (r.point == to_point(pt::work) && r.stage_name == "total") {
            EXPECT_EQ(r.cumulative.count, 1000u);
            found = true;
        }
    }
    EXPECT_TRUE(found) << "the final drain must reach the reports";
}

TEST(CollectorTest, RejectsANonPowerOfTwoCapacity) {
    Collector c;
    config    cfg      = quiet_config();
    cfg.queue_capacity = 1000;
    EXPECT_THROW((void)c.start(cfg), std::invalid_argument);
}

TEST(CollectorTest, RejectsASharedModeWithNoName) {
    Collector c;
    config    cfg = quiet_config();
    cfg.run_mode  = mode::shared_collector;
    EXPECT_THROW((void)c.start(cfg), std::invalid_argument);
}

TEST(CollectorTest, AProducerWithNoCollectorRunningStartsDisabled) {
    Collector c;
    config    cfg = quiet_config();
    cfg.run_mode  = mode::shared_producer;
    cfg.shm_name  = unique_shm_name("nobody");

    EXPECT_FALSE(c.start(cfg)) << "attaching a segment nobody created must fail quietly";
    EXPECT_FALSE(c.enabled());

    // And stamping must stay free and harmless.
    begin(pt::work);
    end(pt::work);
    SUCCEED();
}

TEST(CollectorTest, MeasuresItsOwnOverhead) {
    Collector c;
    config    cfg   = quiet_config();
    cfg.point_count = static_cast<point_id>(pt::count);
    ASSERT_TRUE(c.start(cfg));
    c.flush();

    // Always measured even though it is not subtracted by default: the floor is
    // what says whether a short measurement is real.
    //
    // Only x86 can require it to be non-zero. Elsewhere read_tsc() falls back to
    // steady_clock, which on Apple Silicon resolves to ~41.67 ns - coarser than
    // the stamp being measured. measure_overhead() takes the *minimum* over a
    // thousand pairs, and a minimum over quantised samples reaches zero as soon
    // as one pair lands inside a single tick. Zero is the truthful answer there,
    // "below what this clock can see", not a broken calibration - which is what
    // the upper bound below is for, and that one holds everywhere.
#if SLICK_PERFMON_X86
    EXPECT_GT(c.overhead_cycles(), 0u);
#endif
    EXPECT_LT(c.overhead_cycles(), 100000u) << "an absurd figure means a broken calibration";
    c.shutdown();
}

TEST(CollectorTest, TracksPeakOpenSpans) {
    Collector c;
    config    cfg      = quiet_config();
    cfg.point_count    = static_cast<point_id>(pt::count);
    cfg.max_open_spans = 4096;
    ASSERT_TRUE(c.start(cfg));

    constexpr int kConcurrent = 500;
    for (int i = 0; i < kConcurrent; ++i) {
        begin(pt::work, static_cast<span_seq>(i + 1));
    }
    for (int i = 0; i < kConcurrent; ++i) {
        end(pt::work, static_cast<span_seq>(i + 1));
    }
    c.flush();

    EXPECT_EQ(c.snapshot_total(to_point(pt::work)).count, kConcurrent);
    EXPECT_GE(c.peak_open_spans(), 1u);
    c.shutdown();
}

namespace {

const StageReport* find_total_row(const std::vector<StageReport>& rows, point_id p) {
    for (const StageReport& r : rows) {
        if (r.point == p && r.from == kBeginStep && r.to == kBeginStep) {
            return &r;
        }
    }
    return nullptr;
}

}  // namespace

TEST(CollectorTest, ReusedReportRowsDoNotCarryStaleStatistics) {
    // The flush path refills the rows of an earlier interval in place rather
    // than building a fresh set, so that a steady-state flush allocates
    // nothing. A field left unassigned would then show the previous interval's
    // number - worse than a missing row, because it still reads as a
    // measurement.
    Collector c;
    config    cfg      = quiet_config();
    cfg.point_count    = static_cast<point_id>(pt::count);
    cfg.flush_interval = std::chrono::hours(1);  // only explicit flushes
    ASSERT_TRUE(c.start(cfg));

    constexpr int kFirst = 100;
    for (int i = 0; i < kFirst; ++i) {
        begin(pt::work);
        end(pt::work);
    }
    const std::vector<StageReport> first = c.reports();
    const StageReport*             w1    = find_total_row(first, to_point(pt::work));
    ASSERT_NE(w1, nullptr);
    EXPECT_EQ(w1->interval.count, kFirst);
    EXPECT_GT(w1->interval.mean, 0.0);

    // A second interval in which `work` says nothing at all and a new point
    // takes over. `work`'s row survives, and it must report an empty interval.
    constexpr int kSecond = 7;
    for (int i = 0; i < kSecond; ++i) {
        begin(pt::other);
        end(pt::other);
    }
    const std::vector<StageReport> second = c.reports();

    const StageReport* w2 = find_total_row(second, to_point(pt::work));
    ASSERT_NE(w2, nullptr);
    EXPECT_EQ(w2->interval.count, 0u) << "a silent stage must report an empty interval";
    EXPECT_DOUBLE_EQ(w2->interval.mean, 0.0);
    EXPECT_DOUBLE_EQ(w2->interval.min, 0.0);
    EXPECT_DOUBLE_EQ(w2->interval.max, 0.0);
    for (double p : w2->interval.percentiles) {
        EXPECT_DOUBLE_EQ(p, 0.0);
    }
    EXPECT_EQ(w2->cumulative.count, kFirst) << "the lifetime record must survive";

    const StageReport* o2 = find_total_row(second, to_point(pt::other));
    ASSERT_NE(o2, nullptr);
    EXPECT_EQ(o2->interval.count, kSecond);
    EXPECT_EQ(o2->cumulative.count, kSecond);
    c.shutdown();
}

TEST(CollectorTest, LabelsAndShapeAreStableAcrossManyFlushes) {
    // Labels are resolved once and cached, and the percentile vector of each
    // row is refilled rather than replaced. Both have to survive repetition.
    Collector c;
    config    cfg      = quiet_config();
    cfg.point_count    = static_cast<point_id>(pt::count);
    cfg.flush_interval = std::chrono::hours(1);
    cfg.percentiles    = {50.0, 99.0};
    ASSERT_TRUE(c.start(cfg));

    for (int round = 0; round < 20; ++round) {
        begin(pt::work);
        step(pt::work, kMid);
        end(pt::work);

        const std::vector<StageReport> rows = c.reports();
        ASSERT_FALSE(rows.empty()) << "round " << round;
        for (const StageReport& r : rows) {
            EXPECT_EQ(r.point_name, "point_" + std::to_string(r.point)) << "round " << round;
            EXPECT_FALSE(r.stage_name.empty()) << "round " << round;
            EXPECT_EQ(r.interval.percentiles.size(), cfg.percentiles.size());
            EXPECT_EQ(r.cumulative.percentiles.size(), cfg.percentiles.size());
        }
        const StageReport* total = find_total_row(rows, to_point(pt::work));
        ASSERT_NE(total, nullptr) << "round " << round;
        EXPECT_EQ(total->stage_name, "total");
        EXPECT_EQ(total->interval.count, 1u) << "round " << round;
        EXPECT_EQ(total->cumulative.count, static_cast<uint64_t>(round + 1));
    }
    c.shutdown();
}

TEST(CollectorTest, ReportsItsOwnFlushCost) {
    Collector c;
    config    cfg      = quiet_config();
    cfg.point_count    = static_cast<point_id>(pt::count);
    cfg.flush_interval = std::chrono::hours(1);
    ASSERT_TRUE(c.start(cfg));

    begin(pt::work);
    end(pt::work);
    c.flush();

    EXPECT_GT(c.last_flush_cycles(), 0u)
        << "a flush that built at least one row cannot have cost nothing";
    c.shutdown();
}

TEST(CollectorTest, SubtractOverheadReachesThePairer) {
    // run() measures the stamp overhead and hands it to the Pairer, and the
    // order in which it does so is load-bearing: reset() clears the overhead,
    // so setting it first leaves every session reporting raw timings however
    // config::subtract_overhead was set. Nothing else in the public surface
    // shows that, which is why this is asserted end to end.
    constexpr int kSpans = 20000;

    struct outcome {
        stats    s;
        uint64_t overhead = 0;
    };

    auto run_once = [](bool subtract) {
        Collector c;
        config    cfg         = quiet_config();
        cfg.point_count       = static_cast<point_id>(pt::count);
        cfg.output_unit       = unit::cycles;  // comparable with overhead_cycles()
        cfg.subtract_overhead = subtract;
        EXPECT_TRUE(c.start(cfg));
        for (int i = 0; i < kSpans; ++i) {
            begin(pt::work);
            end(pt::work);
        }
        const outcome out{c.snapshot_total(to_point(pt::work)), c.overhead_cycles()};
        c.shutdown();
        return out;
    };

    const outcome raw = run_once(false);
    const outcome sub = run_once(true);

    ASSERT_EQ(raw.s.count, kSpans);
    ASSERT_EQ(sub.s.count, kSpans);

#if !SLICK_PERFMON_X86
    // The rest cannot be asserted through timing off x86, whatever the pairer
    // does. The fallback clock is coarser than the thing being measured: the
    // calibrated overhead can come out larger than an entire span, record()
    // then clamps the subtraction at zero, and both runs bottom out at zero or
    // one tick whichever way subtract_overhead was set. The mechanism is not
    // platform-specific and is asserted above on x86; what is missing here is a
    // clock able to resolve it.
    GTEST_SKIP() << "the fallback clock is too coarse to resolve one stamp";
#else
    ASSERT_GT(sub.overhead, 0u) << "the floor is always measured";
    ASSERT_GT(raw.s.min, 0.0) << "a fenced stamp pair cannot take zero cycles";

    // Asserted on the minimum rather than the mean: the subtraction is a
    // constant per transition, and the fastest stamp pair on a given machine is
    // a hardware floor, not an average that drifts with how busy the box is.
    EXPECT_LT(sub.s.min, raw.s.min) << "the subtraction never reached the pairer";
    EXPECT_GE(raw.s.min - sub.s.min, static_cast<double>(sub.overhead) / 2.0)
        << "the gap should be of the order of one stamp, not of the noise";
#endif
}

TEST(CollectorTest, RestartDoesNotCarryThePeakForward) {
    // peak_open_spans is a high-water mark, so nothing lowers it during a run.
    // A Collector is restartable, and a peak from the previous session would be
    // reported as this session's.
    Collector c;
    config    cfg      = quiet_config();
    cfg.point_count    = static_cast<point_id>(pt::count);
    cfg.max_open_spans = 64;

    ASSERT_TRUE(c.start(cfg));
    constexpr span_seq kOpen = 16;
    for (span_seq s = 1; s <= kOpen; ++s) {
        begin(pt::work, s);  // opened and deliberately never ended
    }
    (void)c.reports();  // a flush is what publishes the peak
    EXPECT_GE(c.peak_open_spans(), kOpen);
    c.shutdown();

    ASSERT_TRUE(c.start(cfg));
    for (int i = 0; i < 10; ++i) {
        begin(pt::work);
        end(pt::work);  // closed immediately: never more than one span open
    }
    (void)c.reports();
    EXPECT_LE(c.peak_open_spans(), 1u)
        << "the previous session's open spans are not this session's";
    c.shutdown();
}

TEST(CollectorTest, AnomaliesBeyondThePointTableAreStillReported) {
    // The per-point anomaly table is sized like the stage table, so a run that
    // touches more distinct points than max_stages folds the surplus into one
    // shared bucket. That bucket belongs to no point, so it needs a row of its
    // own - without one, the run already past its configured size is the single
    // run whose anomaly counts are invisible.
    Collector c;
    config    cfg   = quiet_config();
    cfg.max_stages  = 4;
    cfg.point_count = 32;
    ASSERT_TRUE(c.start(cfg));

    // An end with no begin is an orphan, and an orphan produces no stage row,
    // so each of these points exists only in the anomaly table.
    for (point_id p = 0; p < 32; ++p) {
        end(p);
    }

    const std::vector<StageReport> rows = c.reports();
    const auto it = std::find_if(rows.begin(), rows.end(), [](const StageReport& r) {
        return r.overflow;
    });
    ASSERT_NE(it, rows.end()) << "anomalies past max_stages points must still surface";
    EXPECT_EQ(it->point_name, "overflow");
    EXPECT_GT(it->cumulative.orphan, 0u);
    c.shutdown();
}

TEST(CollectorTest, TheOverflowRowIsNoPointsRow) {
    // The surplus-anomaly row belongs to no point. It used to say so by
    // carrying the largest point id, which is a perfectly ordinary id: a real
    // stage at that id and the synthetic row then had identical
    // (point, from, to) keys, and snapshot_total() could answer a question
    // about the real one with the other's empty counts. A flag says it
    // instead, because there is no value in a 32-bit space the user owns that
    // can be borrowed for a marker.
    Collector c;
    config    cfg   = quiet_config();
    cfg.max_stages  = 4;
    cfg.point_count = 0;
    ASSERT_TRUE(c.start(cfg));

    constexpr point_id kEdge   = (std::numeric_limits<point_id>::max)();
    constexpr int      kSpans  = 10;
    for (int i = 0; i < kSpans; ++i) {
        begin(kEdge);  // a real point, at the id the marker used to occupy
        end(kEdge);
    }
    // Enough distinct points to fill the per-point anomaly table and overflow
    // it. Each end with no begin is an orphan and produces no stage row.
    for (point_id p = 0; p < 64; ++p) {
        end(p);
    }

    const std::vector<StageReport> rows = c.reports();

    size_t edge_totals = 0;
    for (const StageReport& r : rows) {
        if (!r.overflow && r.point == kEdge && r.from == kBeginStep && r.to == kBeginStep) {
            ++edge_totals;
            EXPECT_EQ(r.cumulative.count, static_cast<uint64_t>(kSpans));
        }
    }
    EXPECT_EQ(edge_totals, 1u) << "one row, and it is the real point's";

    const auto ov = std::find_if(rows.begin(), rows.end(), [](const StageReport& r) {
        return r.overflow;
    });
    ASSERT_NE(ov, rows.end());
    EXPECT_GT(ov->cumulative.orphan, 0u);
    EXPECT_EQ(ov->point_name, "overflow");
    EXPECT_EQ(&*ov, &rows.back()) << "it belongs after every point, not at some id";

    EXPECT_EQ(c.snapshot_total(kEdge).count, static_cast<uint64_t>(kSpans))
        << "the point at that id must answer for itself";
    c.shutdown();
}

TEST(CollectorTest, NonFinitePercentilesAreRefused) {
    // A NaN passes every bounds check a percentile is given, every comparison
    // against it being false, so it would reach the rank arithmetic in the
    // histogram and be cast to an integer - undefined behaviour, from one bad
    // number in a configuration vector.
    Collector c;
    config    cfg = quiet_config();

    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity(),
                       -1.0,
                       100.5}) {
        cfg.percentiles = {50.0, bad};
        EXPECT_THROW((void)c.start(cfg), std::invalid_argument) << "percentile " << bad;
    }

    cfg.percentiles = {0.0, 50.0, 100.0};
    EXPECT_TRUE(c.start(cfg)) << "both ends of the range are valid";
    c.shutdown();
}
