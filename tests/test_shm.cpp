// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// Cross-process collection. This test really does launch a second process and
// attach the same segment from another address space, because that is the
// mode the library exists for and nothing in-process can stand in for it.

#include <slick/perfmon.hpp>
#include <slick/perfmon/collector.hpp>

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace slick::perfmon;
using namespace slick::perfmon::test;

namespace {

constexpr point_id kPointA = 0;
constexpr point_id kPointB = 100;
constexpr uint8_t  kMid    = 2;

constexpr std::string_view names(point_id p, uint8_t step) noexcept {
    if (p == kPointA) {
        switch (step) {
        case kBeginStep: return "feed";
        case kMid:       return "decode";
        case kEndStep:   return "sent";
        default:         return {};
        }
    }
    if (p == kPointB && step == kBeginStep) {
        return "strategy";
    }
    return {};
}

/// Run the helper producer and return its exit code.
int run_producer(const std::string& shm, point_id point, int spans,
                 const std::string& flags = "") {
    std::string cmd = "\"";
    cmd += PRODUCER_EXE_PATH;
    cmd += "\" ";
    cmd += shm;
    cmd += " " + std::to_string(point);
    cmd += " " + std::to_string(spans);
    if (!flags.empty()) {
        cmd += " " + flags;
    }
#if defined(_WIN32)
    // cmd.exe strips one layer of quoting from a command that begins with a
    // quote, so the whole line needs wrapping again.
    cmd = "\"" + cmd + "\"";
#endif
    return std::system(cmd.c_str());
}

/// A collector that owns the segment, as the real deployment has it: the
/// collector creates, producers attach.
config collector_config(const std::string& shm) {
    config cfg      = quiet_config();
    cfg.run_mode    = mode::shared_collector;
    cfg.shm_name    = shm;
    cfg.point_count = kPointB + 1;
    cfg.name_of     = &names;
    return cfg;
}

}  // namespace

TEST(Shm, CollectsSpansFromAnotherProcess) {
    const std::string shm = unique_shm_name("slick_perfmon_basic");

    Collector c;
    ASSERT_TRUE(c.start(collector_config(shm)));

    constexpr int kSpans = 2000;
    ASSERT_EQ(run_producer(shm, kPointA, kSpans, "--steps"), 0);

    c.flush();
    const stats total = c.snapshot_total(kPointA);
    EXPECT_EQ(total.count, kSpans);
    EXPECT_EQ(total.dropped, 0u);
    EXPECT_EQ(total.orphan, 0u);
    EXPECT_EQ(total.abandoned, 0u);

    // The intermediate step has to survive the process boundary too.
    EXPECT_EQ(c.snapshot(kPointA, kBeginStep, kMid).count, kSpans);
    EXPECT_EQ(c.snapshot(kPointA, kMid, kEndStep).count, kSpans);
    c.shutdown();
}

TEST(Shm, NamesPublishedByTheCollectorLabelAnotherProcessSpans) {
    const std::string shm = unique_shm_name("slick_perfmon_names2");

    Collector c;
    ASSERT_TRUE(c.start(collector_config(shm)));
    ASSERT_EQ(run_producer(shm, kPointA, 200, "--steps"), 0);

    bool saw_feed = false, saw_decode = false;
    for (const StageReport& r : c.reports()) {
        if (r.point == kPointA) {
            EXPECT_EQ(r.point_name, "feed");
            saw_feed = true;
            if (r.stage_name == "decode") {
                saw_decode = true;
            }
        }
    }
    EXPECT_TRUE(saw_feed);
    EXPECT_TRUE(saw_decode) << "step names must resolve for a remote producer";
    c.shutdown();
}

TEST(Shm, TwoProducersOnDisjointPointRangesDoNotInterfere) {
    // The documented way to keep processes apart: partition the id space.
    const std::string shm = unique_shm_name("slick_perfmon_two");

    Collector c;
    ASSERT_TRUE(c.start(collector_config(shm)));

    constexpr int kSpansA = 1500;
    constexpr int kSpansB = 900;

    std::thread a([&] { run_producer(shm, kPointA, kSpansA); });
    std::thread b([&] { run_producer(shm, kPointB, kSpansB); });
    a.join();
    b.join();

    c.flush();
    EXPECT_EQ(c.snapshot_total(kPointA).count, kSpansA);
    EXPECT_EQ(c.snapshot_total(kPointB).count, kSpansB);
    EXPECT_EQ(c.snapshot_total(kPointA).orphan, 0u);
    EXPECT_EQ(c.snapshot_total(kPointB).orphan, 0u);
    c.shutdown();
}

TEST(Shm, TwoProducersSharingOnePointMergeIntoOneRow) {
    // The other documented option: share an id deliberately so the numbers
    // combine. Each process takes its own seq range, so the spans stay
    // distinguishable even though they land on the same point.
    const std::string shm = unique_shm_name("slick_perfmon_merge");

    Collector c;
    ASSERT_TRUE(c.start(collector_config(shm)));

    constexpr int kEach = 800;
    std::thread   a([&] { run_producer(shm, kPointA, kEach, "--seq-base 0"); });
    std::thread   b([&] { run_producer(shm, kPointA, kEach, "--seq-base 100000"); });
    a.join();
    b.join();

    c.flush();
    const stats total = c.snapshot_total(kPointA);
    EXPECT_EQ(total.count, static_cast<uint64_t>(kEach) * 2)
        << "both processes should land in the same row, with nothing lost";
    EXPECT_EQ(total.abandoned, 0u) << "disjoint seq ranges must not collide";
    EXPECT_EQ(total.orphan, 0u);
    c.shutdown();
}

TEST(Shm, TwoProducersWithCollidingSeqsAreDetectedNotMispaired) {
    // The failure mode of the test above: both processes using seq 1..N on one
    // point. What matters is that it shows up in the anomaly counters rather
    // than as a believable latency.
    const std::string shm = unique_shm_name("slick_perfmon_collide");

    Collector c;
    ASSERT_TRUE(c.start(collector_config(shm)));

    constexpr int kEach = 800;
    std::thread   a([&] { run_producer(shm, kPointA, kEach); });
    std::thread   b([&] { run_producer(shm, kPointA, kEach); });
    a.join();
    b.join();

    c.flush();
    const stats total = c.snapshot_total(kPointA);

    // Every begin opens a span; a span leaves the table either by being closed
    // (count) or by being clobbered (abandoned). Whatever is left is still
    // open, so the two can never add up to more than the begins issued.
    EXPECT_LE(total.count + total.abandoned, static_cast<uint64_t>(kEach) * 2);
    EXPECT_GT(total.count, 0u);
    c.shutdown();
}

TEST(Shm, SpansLeftOpenByAProducerAreEventuallyAbandoned) {
    // A producer that exits mid-span - or dies - must not leak an open-span
    // slot for the life of the collector.
    const std::string shm = unique_shm_name("slick_perfmon_leak");

    config cfg                 = collector_config(shm);
    cfg.stalled_sample_timeout = std::chrono::milliseconds(50);
    cfg.flush_interval         = std::chrono::milliseconds(20);

    Collector c;
    ASSERT_TRUE(c.start(cfg));

    constexpr int kSpans = 100;
    ASSERT_EQ(run_producer(shm, kPointA, kSpans, "--leave-open"), 0);

    c.flush();
    EXPECT_EQ(c.snapshot_total(kPointA).count, 0u) << "nothing completed";
    EXPECT_GE(c.peak_open_spans(), 1u);

    // Let the age sweep run.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    c.flush();

    EXPECT_GT(c.snapshot_total(kPointA).abandoned, 0u)
        << "abandoned spans must be swept and counted, not leaked";
    c.shutdown();
}

TEST(Shm, AProducerStartedBeforeTheCollectorRecordsNothingAndSaysSo) {
    // The safe default: an instrumented binary in production with no collector
    // attached costs nothing and creates nothing.
    const std::string shm = unique_shm_name("slick_perfmon_early");

    const int rc = run_producer(shm, kPointA, 100);
    EXPECT_NE(rc, 0) << "the producer should report that no collector was there";

    // And the segment must not have been created as a side effect, so a
    // collector starting now is the creator.
    Collector c;
    EXPECT_TRUE(c.start(collector_config(shm)));
    EXPECT_EQ(c.snapshot_total(kPointA).count, 0u);
    c.shutdown();
}

TEST(Shm, AttachingWithAMismatchedNameTableThrows) {
    const std::string shm = unique_shm_name("slick_perfmon_badcfg");

    Collector collector;
    ASSERT_TRUE(collector.start(collector_config(shm)));

    // A peer built or configured differently would read the name table at the
    // wrong stride, so this has to fail loudly.
    Collector peer;
    config    bad  = quiet_config();
    bad.run_mode   = mode::shared_producer;
    bad.shm_name   = shm;
    bad.max_names  = 999;
    EXPECT_THROW((void)peer.start(bad), std::runtime_error);

    collector.shutdown();
}

TEST(Shm, AProducerCanCreateTheSegmentWhenAskedTo) {
    // The escape hatch for the reverse startup order.
    const std::string shm = unique_shm_name("slick_perfmon_create");

    Collector producer;
    config    cfg       = quiet_config();
    cfg.run_mode        = mode::shared_producer;
    cfg.shm_name        = shm;
    cfg.create_if_absent = true;
    cfg.point_count     = kPointB + 1;
    cfg.name_of         = &names;
    ASSERT_TRUE(producer.start(cfg));
    EXPECT_TRUE(producer.enabled());

    for (int i = 0; i < 100; ++i) {
        begin(kPointA, static_cast<span_seq>(i + 1));
        end(kPointA, static_cast<span_seq>(i + 1));
    }

    // No collector thread in producer mode, so nothing is paired here - the
    // events simply wait in the segment.
    EXPECT_EQ(producer.snapshot_total(kPointA).count, 0u);
    producer.shutdown();
}

namespace {

constexpr std::string_view renamed(point_id p, uint8_t step) noexcept {
    if (p == kPointA && step == kBeginStep) {
        return "renamed_feed";
    }
    return {};
}

}  // namespace

TEST(Shm, ANameIsReusableWithADifferentConfigurationAfterShutdown) {
    // Regression, POSIX: the control block segment was unmapped but never
    // unlinked, so the name outlived the process that created it. A rerun with
    // a different max_names then hit the peer-mismatch throw against nothing
    // but its own corpse. On Windows the segment is refcounted by handle and
    // this already held, which is precisely why it needed a test.
    const std::string shm = unique_shm_name("slick_perfmon_reuse");

    {
        Collector c;
        config    cfg = collector_config(shm);
        cfg.max_names = 64;
        ASSERT_TRUE(c.start(cfg));
        c.shutdown();
    }

    Collector second;
    config    cfg = collector_config(shm);
    cfg.max_names = 128;  // deliberately different from the first session
    EXPECT_NO_THROW({ ASSERT_TRUE(second.start(cfg)); });
    second.shutdown();
}

TEST(Shm, AReusedNameDoesNotResurrectTheOldSessionsLabels) {
    // The other half of the leak: a stale block kept its name table, so a rerun
    // that renamed a point silently reported it under the previous run's label.
    const std::string shm = unique_shm_name("slick_perfmon_relabel");

    {
        Collector c;
        ASSERT_TRUE(c.start(collector_config(shm)));  // publishes "feed"
        c.shutdown();
    }

    Collector second;
    config    cfg = collector_config(shm);
    cfg.name_of   = &renamed;
    ASSERT_TRUE(second.start(cfg));

    for (int i = 0; i < 100; ++i) {
        begin(kPointA, static_cast<span_seq>(i + 1));
        end(kPointA, static_cast<span_seq>(i + 1));
    }
    second.flush();

    bool found = false;
    for (const StageReport& r : second.reports()) {
        if (r.point == kPointA) {
            EXPECT_EQ(r.point_name, "renamed_feed")
                << "the previous session's label survived in a stale segment";
            found = true;
        }
    }
    EXPECT_TRUE(found);
    second.shutdown();
}
