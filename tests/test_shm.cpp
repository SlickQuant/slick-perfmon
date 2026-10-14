// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// Cross-process collection. This test really does launch a second process and
// attach the same segment from another address space, because that is the
// mode the library exists for and nothing in-process can stand in for it.

#include <slick/perfmon.hpp>
#include <slick/perfmon/collector.hpp>

#include "shm_test_support.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
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
    const scoped_shm_name shm("basic");

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
    const scoped_shm_name shm("names2");

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
    const scoped_shm_name shm("two");

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
    const scoped_shm_name shm("merge");

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
    const scoped_shm_name shm("collide");

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
    const scoped_shm_name shm("leak");

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
    const scoped_shm_name shm("early");

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
    const scoped_shm_name shm("badcfg");

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

TEST(Shm, AnOverLongShmNameIsRefusedByName) {
    // The bug this guards: nothing checked the name against the platform limit,
    // so on macOS - where PSHMNAMLEN caps a shm_open() name at 31 bytes against
    // 249 elsewhere - the ring segment would open under `name` while the control
    // block one suffix longer would not. What reached the caller was a bare
    // false from a half-built session, or "File name too long" thrown out of a
    // dependency. Neither said which knob was wrong, and neither happened on the
    // machine the config was written on.
    Collector c;
    config    cfg = quiet_config();
    cfg.run_mode  = mode::shared_producer;
    cfg.shm_name  = std::string(SLICK_PERFMON_SHM_NAME_MAX + 1, 'x');

    EXPECT_THROW((void)c.start(cfg), std::invalid_argument);

    // And the boundary itself is accepted, so the limit is off-by-one clean.
    // create_if_absent because there is no collector on this name to attach to.
    config ok           = quiet_config();
    ok.run_mode         = mode::shared_producer;
    ok.create_if_absent = true;
    const scoped_shm_name edge_name("edge");
    ok.shm_name = edge_name;
    ASSERT_LE(ok.shm_name.size(), static_cast<size_t>(SLICK_PERFMON_SHM_NAME_MAX));
    Collector edge;
    EXPECT_TRUE(edge.start(ok)) << "a name inside the budget must open both segments";
    edge.shutdown();
}

TEST(Shm, UniqueShmNameStaysInsideThePlatformBudget) {
    // unique_shm_name() is what keeps every shm test inside the platform limit,
    // and an over-long name fails only on macOS - so without this the regression
    // comes back as a red CI job on one platform months later.
    //
    // The long tags are the point: the helper has to truncate rather than hand
    // back something the platform will reject. "slick_perfmon_early" is the real
    // one from the run that found this, when names were built as the tag plus a
    // *decimal* suffix - 26 to 33 characters against a budget of 25, and varying
    // in width, so the same test passed or failed on the draw.
    for (const char* tag : {"basic", "collide", "generation", "slick_perfmon_early",
                            "a_tag_far_longer_than_any_platform_would_accept"}) {
        const std::string name = unique_shm_name(tag);
        EXPECT_LE(name.size(), static_cast<size_t>(SLICK_PERFMON_SHM_NAME_MAX))
            << "tag " << tag << " produced " << name;
        EXPECT_FALSE(name.empty());
        EXPECT_NE(name, unique_shm_name(tag)) << "names must stay unique after truncation";
    }
}

TEST(Shm, AProducerCanCreateTheSegmentWhenAskedTo) {
    // The escape hatch for the reverse startup order.
    const scoped_shm_name shm("create");

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

TEST(Shm, ANameIsReusableWithADifferentConfigurationOnceRemoved) {
    // A session's segments are never unlinked implicitly, so that a collector
    // can restart under producers that are still running. The other side of
    // that: on POSIX the name outlives the process that created it, bound to
    // the settings it was created with, and a rerun with a different max_names
    // is the peer mismatch it would be against a live block. Removing the name
    // is what gives it a clean start. On Windows the segment went with its
    // last handle and the removal is a no-op.
    const scoped_shm_name shm("reuse");

    {
        Collector c;
        config    cfg = collector_config(shm);
        cfg.max_names = 64;
        ASSERT_TRUE(c.start(cfg));
        c.shutdown();
    }
    Collector::remove_shm(shm);

    Collector second;
    config    cfg = collector_config(shm);
    cfg.max_names = 128;  // deliberately different from the first session
    EXPECT_NO_THROW({ ASSERT_TRUE(second.start(cfg)); });
    second.shutdown();
}

TEST(Shm, ACollectorRestartsOnTheSameNameWithADifferentCapacity) {
    // Regression: shutdown() retires the ring rather than freeing it, and a
    // retired shared ring kept its segment - and with it the name - alive. The
    // restart then opened the previous session's segment instead of creating
    // its own, and threw "Shared memory size mismatch" against nothing but
    // this Collector's own leftover. The test above uses two Collectors, so it
    // never had a retired ring in the way.
    //
    // Freeing the retired ring is still what makes this work on Windows. On
    // POSIX the segment is never unlinked implicitly, so a different capacity
    // also needs the name removed first - while the retired ring is still
    // mapped, which removal has to be safe against.
    const scoped_shm_name shm("recap");

    Collector c;
    config    first     = collector_config(shm);
    first.queue_capacity = 1024;
    ASSERT_TRUE(c.start(first));
    c.shutdown();
    Collector::remove_shm(shm);

    config second         = collector_config(shm);
    second.queue_capacity = 4096;
    EXPECT_NO_THROW({ ASSERT_TRUE(c.start(second)); });
    c.shutdown();
}

TEST(Shm, ARestartedCollectorDrainsItsOwnSegment) {
    // The quieter half of the same bug: with an unchanged capacity the restart
    // does not throw. Whichever segment it ends up on - a fresh one, or the
    // previous session's, which is the one it finds wherever that is still
    // there - what has to hold is that a producer arriving after the restart
    // lands where it is drained.
    const scoped_shm_name shm("redrain");

    Collector c;
    ASSERT_TRUE(c.start(collector_config(shm)));
    c.shutdown();
    ASSERT_TRUE(c.start(collector_config(shm)));

    constexpr int kSpans = 500;
    ASSERT_EQ(run_producer(shm, kPointA, kSpans), 0);

    c.flush();
    const stats total = c.snapshot_total(kPointA);
    EXPECT_EQ(total.count, kSpans);
    EXPECT_EQ(total.orphan, 0u);
    c.shutdown();
}

TEST(Shm, ARemovedNameDoesNotResurrectTheOldSessionsLabels) {
    // A block left in place keeps its name table - on purpose, for the sake of
    // producers that outlive a collector. So a rerun that renames a point has
    // to remove the name first, or it reports the point under the previous
    // run's label: the first name published for a key wins.
    const scoped_shm_name shm("relabel");

    {
        Collector c;
        ASSERT_TRUE(c.start(collector_config(shm)));  // publishes "feed"
        c.shutdown();
    }
    Collector::remove_shm(shm);

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

namespace {

/// Whether a session's ring segment exists. Attach-only: this constructor
/// opens a segment that is there and throws when there is none, which is what
/// a late producer relies on to learn that no collector is running.
bool ring_exists(const std::string& shm) {
    try {
        sample_queue probe(shm.c_str());
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// Whether a session's control block segment exists.
bool control_block_exists(const std::string& shm) {
    ControlBlockOwner probe;
    return probe.attach_shared(shm + SLICK_PERFMON_META_SUFFIX, config{}.max_names);
}

constexpr std::string_view strategy_only(point_id p, uint8_t step) noexcept {
    return p == kPointB && step == kBeginStep ? "strategy" : std::string_view{};
}

}  // namespace

TEST(Shm, AShmTestLeavesNoSegmentBehind) {
    // Regression: nothing in the library unlinks a segment - slick-queue
    // stopped doing it for the ring in 2.3.0, and the Collector does not do it
    // for either - so on POSIX every test in this file left its segments in
    // /dev/shm for good. scoped_shm_name removes them explicitly; this is the
    // check that it does.
    //
    // On Windows a segment goes with its last handle, so this holds there with
    // or without the guard. It is Linux and macOS it is here for.
    std::string name;
    {
        const scoped_shm_name shm("gone");
        name = shm.str();

        Collector c;
        ASSERT_TRUE(c.start(collector_config(shm)));
        c.shutdown();
    }

    EXPECT_FALSE(ring_exists(name))
        << "the ring segment '" << name << "' outlived the test that created it";
    EXPECT_FALSE(control_block_exists(name))
        << "the control block of '" << name << "' outlived the test that created it";
}

TEST(Shm, ACollectorRestartKeepsARunningProducersSegmentsAndNames) {
    // What keeping the segments is for. A collector that unlinked them on its
    // way out left every producer still running attached to segments nobody
    // would open again: the next collector made fresh ones under the same
    // name, drained nothing those producers stamped, and had none of the
    // labels they published. Here the producer outlives the collector, and the
    // collector that replaces it has to find both the stamps and the names.
    const scoped_shm_name shm("survive");

    auto first = std::make_unique<Collector>();
    ASSERT_TRUE(first->start(collector_config(shm)));

    // The producer names kPointB itself. The replacement collector below is
    // given no name table at all, so the label can only reach its report from
    // the block this producer published into.
    Collector producer;
    config    pcfg   = quiet_config();
    pcfg.run_mode    = mode::shared_producer;
    pcfg.shm_name    = shm;
    pcfg.point_count = kPointB + 1;
    pcfg.name_of     = &strategy_only;
    ASSERT_TRUE(producer.start(pcfg));

    // The mapping the producer attached while the first collector was alive,
    // and the only one it will ever have: a running producer does not reopen
    // the segment because the collector changed.
    sample_queue* const kept = producer.ring();
    ASSERT_NE(kept, nullptr);

    first->shutdown();
    first.reset();  // the first collector is gone, mappings and all

    Collector second;
    config    scfg   = collector_config(shm);
    scfg.point_count = 0;
    scfg.name_of     = nullptr;
    ASSERT_TRUE(second.start(scfg));
    ASSERT_NE(second.ring(), kept) << "the replacement has a mapping of its own";

    // Published through the producer's mapping explicitly, with the stamp the
    // hot path itself uses. begin() and end() would not do: this process has
    // one hot-path pointer, second.start() has just pointed it at the
    // replacement's own mapping, and stamps sent that way arrive whatever
    // segment the replacement opened - a fresh one included, which is exactly
    // the failure this test exists to catch. Only what goes in through `kept`
    // says whether the two are still the same segment.
    constexpr int kSpans = 300;
    for (int i = 0; i < kSpans; ++i) {
        const span_seq seq = static_cast<span_seq>(i + 1);
        detail::stamp_into<true>(*kept, kPointB, make_event(kBeginStep, seq));
        detail::stamp_into<false>(*kept, kPointB, make_event(kEndStep, seq));
    }

    const stats total = second.snapshot_total(kPointB);
    EXPECT_EQ(total.count, kSpans) << "the restarted collector is not draining the ring "
                                      "the producer is still attached to";

    bool found = false;
    for (const StageReport& r : second.reports()) {
        if (r.point == kPointB) {
            EXPECT_EQ(r.point_name, "strategy")
                << "the name the producer published did not survive the restart";
            found = true;
        }
    }
    EXPECT_TRUE(found);

    second.shutdown();
    producer.shutdown();
}

#if !defined(_WIN32)

TEST(Shm, SegmentsOutliveTheirCollectorUntilRemoved) {
    // The POSIX half of the contract, stated on its own: nothing a Collector
    // does on the way out unlinks anything, and remove_shm() is what does.
    // Not on Windows, where a segment cannot outlive its last handle and there
    // is no name to remove.
    const scoped_shm_name shm("outlive");

    {
        Collector c;
        ASSERT_TRUE(c.start(collector_config(shm)));
        c.shutdown();
    }
    EXPECT_TRUE(ring_exists(shm)) << "the ring was unlinked by its Collector";
    EXPECT_TRUE(control_block_exists(shm)) << "the control block was unlinked by its Collector";

    Collector::remove_shm(shm);
    EXPECT_FALSE(ring_exists(shm));
    EXPECT_FALSE(control_block_exists(shm));

    Collector::remove_shm(shm);  // a name that is not there is not an error
}

#endif  // !_WIN32
