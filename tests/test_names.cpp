// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// Names replace the registration table an earlier design would have needed.
// They are pushed in bulk at start() from the user's own constexpr function,
// which is why these tests exercise ControlBlockOwner directly.

#include <slick/perfmon/control_block.hpp>

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace slick::perfmon;
using namespace slick::perfmon::test;

namespace {

constexpr uint8_t kDecode = 1;
constexpr uint8_t kMatch  = 2;

/// Names points 0 and 1, and steps only on point 0 - so the tests can tell
/// "named" from "deliberately unnamed".
constexpr std::string_view names(point_id p, uint8_t step) noexcept {
    if (p == 0) {
        switch (step) {
        case kBeginStep: return "tick_to_trade";
        case kDecode:    return "decode";
        case kMatch:     return "match";
        case kEndStep:   return "done";
        default:         return {};
        }
    }
    if (p == 1 && step == kBeginStep) {
        return "book_update";
    }
    return {};
}

constexpr std::string_view over_long(point_id p, uint8_t step) noexcept {
    if (p == 0 && step == kBeginStep) {
        // One character past the slot, including its NUL.
        return "0123456789012345678901234567890123456789"
               "0123456789012345678901234";
    }
    return {};
}

}  // namespace

/// A second table, for the case where a peer publishes names after the
/// collector already resolved a label from what was there.
constexpr std::string_view more_names(point_id p, uint8_t step) noexcept {
    if (p == 3 && step == kBeginStep) {
        return "late_arrival";
    }
    return {};
}

/// Two tables that disagree about one key, for the claim race. Nothing else is
/// named, so both publishers contend for the same slot and nothing else.
constexpr std::string_view one_name_alpha(point_id p, uint8_t step) noexcept {
    return (p == 0 && step == kBeginStep) ? std::string_view("alpha") : std::string_view();
}

constexpr std::string_view one_name_beta(point_id p, uint8_t step) noexcept {
    return (p == 0 && step == kBeginStep) ? std::string_view("beta") : std::string_view();
}

TEST(Names, PublishesPointsAndSteps) {
    ControlBlockOwner cb;
    cb.create_local(64);
    EXPECT_EQ(cb.publish_names(2, &names), 0u);

    EXPECT_EQ(cb.lookup(0, kBeginStep), "tick_to_trade");
    EXPECT_EQ(cb.lookup(0, kDecode), "decode");
    EXPECT_EQ(cb.lookup(0, kMatch), "match");
    EXPECT_EQ(cb.lookup(0, kEndStep), "done");
    EXPECT_EQ(cb.lookup(1, kBeginStep), "book_update");
}

TEST(Names, UnnamedEntriesStayEmptyRatherThanConsumingASlot) {
    ControlBlockOwner cb;
    cb.create_local(64);
    cb.publish_names(2, &names);

    // Point 1 has no step names, and point 0 has no name for step 7.
    EXPECT_TRUE(cb.lookup(1, kDecode).empty());
    EXPECT_TRUE(cb.lookup(0, 7).empty());
    EXPECT_TRUE(cb.lookup(5, kBeginStep).empty());
}

TEST(Names, ANullNameFunctionIsValid) {
    // Useful for a one-off measurement; the collector falls back to
    // point_<p> / step_<a>-><b>.
    ControlBlockOwner cb;
    cb.create_local(16);
    EXPECT_EQ(cb.publish_names(4, nullptr), 0u);
    EXPECT_TRUE(cb.lookup(0, kBeginStep).empty());
}

TEST(Names, OverLongNamesAreRejectedNotTruncated) {
    // Truncation would silently alias two different points onto one row, which
    // is worse than having no label at all.
    ControlBlockOwner cb;
    cb.create_local(16);
    EXPECT_EQ(cb.publish_names(1, &over_long), 1u) << "the rejection must be reported";
    EXPECT_TRUE(cb.lookup(0, kBeginStep).empty());
}

TEST(Names, RepublishingTheSameNameIsNotAConflict) {
    ControlBlockOwner cb;
    cb.create_local(64);
    EXPECT_EQ(cb.publish_names(2, &names), 0u);
    EXPECT_EQ(cb.publish_names(2, &names), 0u) << "idempotent, as a restart would be";
    EXPECT_EQ(cb.lookup(0, kDecode), "decode");
}

TEST(Names, ADisagreeingNameIsReportedAndTheFirstOneWins) {
    ControlBlockOwner cb;
    cb.create_local(64);
    cb.publish_names(2, &names);

    // A second process that disagrees about what point 0 is called.
    constexpr auto other = [](point_id p, uint8_t step) noexcept -> std::string_view {
        if (p == 0 && step == kBeginStep) {
            return "something_else";
        }
        return {};
    };
    EXPECT_EQ(cb.publish_names(1, other), 1u);
    EXPECT_EQ(cb.lookup(0, kBeginStep), "tick_to_trade")
        << "the first publisher's label must stand, rather than flip-flopping";
}

TEST(Names, RunningOutOfSlotsIsReported) {
    ControlBlockOwner cb;
    cb.create_local(2);
    // Four names for two slots.
    EXPECT_GT(cb.publish_names(2, &names), 0u);
}

TEST(Names, NameKeyIsUniquePerPointAndStep) {
    EXPECT_EQ(name_key(0, kBeginStep), 0u);
    EXPECT_NE(name_key(0, 1), name_key(1, 0));
    EXPECT_NE(name_key(1, 0), name_key(0, 1));
    for (uint32_t s = 0; s <= 0xFF; ++s) {
        EXPECT_EQ(name_key(7, static_cast<uint8_t>(s)), (7ull << 8) | s);
    }
}

TEST(Names, CalibrationRoundTripsThroughTheControlBlock) {
    ControlBlockOwner cb;
    cb.create_local(8);
    cb.publish_calibration(2'995'200'000.0, true);
    EXPECT_NEAR(cb.published_hz(), 2'995'200'000.0, 1.0);
}

TEST(Names, AttachingASegmentThatDoesNotExistFailsQuietly) {
    // The normal "no collector is running" case. It must be cheap and silent,
    // not an exception.
    ControlBlockOwner cb;
    EXPECT_FALSE(cb.attach_shared(unique_shm_name("slick_perfmon_absent"), 64));
    EXPECT_FALSE(static_cast<bool>(cb));
}

TEST(Names, SharedSegmentsAgreeAcrossOwners) {
    const std::string name = unique_shm_name("slick_perfmon_names");

    ControlBlockOwner creator;
    ASSERT_TRUE(creator.create_shared(name, 64));
    creator.publish_names(2, &names);
    creator.publish_calibration(3'000'000'000.0, true);

    ControlBlockOwner attacher;
    ASSERT_TRUE(attacher.attach_shared(name, 64));
    EXPECT_EQ(attacher.lookup(0, kDecode), "decode");
    EXPECT_EQ(attacher.lookup(1, kBeginStep), "book_update");
    EXPECT_NEAR(attacher.published_hz(), 3'000'000'000.0, 1.0);
}

TEST(Names, AttachingWithAMismatchedCapacityThrows) {
    // A peer configured differently would read the name table at the wrong
    // stride, so this has to fail loudly rather than return nonsense.
    const std::string name = unique_shm_name("slick_perfmon_mismatch");

    ControlBlockOwner creator;
    ASSERT_TRUE(creator.create_shared(name, 64));

    ControlBlockOwner attacher;
    EXPECT_THROW((void)attacher.attach_shared(name, 128), std::runtime_error);
}

TEST(Names, ClaimedNamesCountsWhatHasBeenPublished) {
    // Occupancy, which is not the same as readability - see
    // GenerationMovesOnlyWhenANameBecomesReadable for the distinction and for
    // why the collector's label cache keys on the generation instead.
    ControlBlockOwner cb;
    cb.create_local(64);
    EXPECT_EQ(cb.claimed_names(), 0u);

    ASSERT_EQ(cb.publish_names(2, &names), 0u);
    const uint32_t after_first = cb.claimed_names();
    EXPECT_EQ(after_first, 5u) << "the fixture names five (point, step) pairs";

    // Republishing the same table claims nothing further.
    ASSERT_EQ(cb.publish_names(2, &names), 0u);
    EXPECT_EQ(cb.claimed_names(), after_first);
}

TEST(Names, ClaimedNamesGrowsWhenAPeerPublishesLater) {
    const std::string name = slick::perfmon::test::unique_shm_name("perfmon_claimed");

    ControlBlockOwner collector;
    ASSERT_TRUE(collector.create_shared(name, 64));
    ASSERT_EQ(collector.publish_names(2, &names), 0u);
    const uint32_t before = collector.claimed_names();
    EXPECT_GT(before, 0u);

    // A second owner attaches the same segment and adds a name of its own,
    // the way a producer process joining a running collector does.
    ControlBlockOwner producer;
    ASSERT_TRUE(producer.attach_shared(name, 64));
    ASSERT_EQ(producer.publish_names(4, &more_names), 0u);

    EXPECT_GT(collector.claimed_names(), before)
        << "the collector must be able to see that new names arrived";
    EXPECT_EQ(collector.lookup(3, kBeginStep), "late_arrival");
}

TEST(Names, GenerationMovesOnlyWhenANameBecomesReadable) {
    // The collector caches resolved labels rather than re-scanning this table
    // twice per row per flush, and needs to know when one it resolved to a
    // "point_7" fallback has since been published.
    //
    // claimed_names() cannot answer that. A slot is claimed before its name is
    // written and skipped by lookup() until afterwards, so a count taken inside
    // that window already includes a name the cache could not read - and then
    // never moves again when the name lands, leaving the fallback in every row
    // for the life of the process. The generation moves only on publication.
    ControlBlockOwner cb;
    cb.create_local(64);
    EXPECT_EQ(cb.name_generation(), 0u);

    ASSERT_EQ(cb.publish_names(2, &names), 0u);
    const uint64_t after_first = cb.name_generation();
    EXPECT_EQ(after_first, 5u) << "one per published name; the fixture names five";
    EXPECT_EQ(after_first, cb.claimed_names())
        << "with no concurrent publisher the two agree, which is why the bug hid";

    ASSERT_EQ(cb.publish_names(2, &names), 0u);
    EXPECT_EQ(cb.name_generation(), after_first) << "republishing publishes nothing new";
}

TEST(Names, GenerationSeesAPeerPublishingLater) {
    const std::string name = unique_shm_name("perfmon_generation");

    ControlBlockOwner collector;
    ASSERT_TRUE(collector.create_shared(name, 64));
    ASSERT_EQ(collector.publish_names(2, &names), 0u);
    const uint64_t before = collector.name_generation();
    EXPECT_GT(before, 0u);

    ControlBlockOwner producer;
    ASSERT_TRUE(producer.attach_shared(name, 64));
    ASSERT_EQ(producer.publish_names(4, &more_names), 0u);

    EXPECT_EQ(collector.name_generation(), before + 1)
        << "one name arrived, and the reader has to be able to see it";
    EXPECT_EQ(collector.lookup(3, kBeginStep), "late_arrival");
}

TEST(Names, AStalledClaimantStillCannotSplitAKeyAcrossTwoSlots) {
    // The race above is won or lost in microseconds. This is the other end of
    // it: a claimant preempted - or killed outright - between claiming the slot
    // and storing the key it claimed it for. A bounded wait has to give up on
    // one, and when it did, the waiter took the slot for somebody else's and
    // published the same key into a second slot, which splits one key across
    // two rows and hides a genuine disagreement over the name. The claim
    // carries its key, so the case is decided without waiting for anybody.
    ControlBlockOwner cb;
    cb.create_local(8);

    name_entry*    tbl  = cb.block()->names();
    const uint64_t want = name_key(0, kBeginStep) + 1;
    tbl[0].key_plus_one.store(want | control_block::kClaimBit, std::memory_order_release);

    EXPECT_EQ(cb.publish_names(1, &one_name_alpha), 1u)
        << "a name that cannot be compared is a disagreement, not a silent pass";
    EXPECT_EQ(cb.claimed_names(), 1u) << "the key must not take a second slot";
    EXPECT_TRUE(cb.lookup(0, kBeginStep).empty()) << "a claimed slot stays unreadable";
    EXPECT_EQ(cb.name_generation(), 0u) << "nothing became readable";

    // And a leaked claim must not stop anybody else from publishing: the key
    // in the claim says the slot is not theirs, so they move straight past it.
    ASSERT_EQ(cb.publish_names(4, &more_names), 0u);
    EXPECT_EQ(cb.lookup(3, kBeginStep), "late_arrival");
    EXPECT_EQ(cb.claimed_names(), 2u);

    // The stalled claimant finishing late is still correct: it owns the slot,
    // and its name becomes readable when it gets there.
    std::memcpy(tbl[0].name, "alpha", 6);
    tbl[0].key_plus_one.store(want, std::memory_order_release);
    EXPECT_EQ(cb.lookup(0, kBeginStep), "alpha");
    EXPECT_EQ(cb.publish_names(1, &one_name_alpha), 0u) << "now it agrees";
    EXPECT_EQ(cb.claimed_names(), 2u) << "and still occupies one slot";
}

TEST(Names, ConcurrentPublishersOfOneKeyClaimOneSlot) {
    // Two processes publishing the same (point, step) race for one slot. The
    // loser used to skip the slot without waiting to learn which key had won
    // it - mid-claim the key reads as kClaiming and says nothing - so one key
    // could end up occupying two slots, and a genuine disagreement over the
    // name went uncounted, the comparison that reports it being the one
    // skipped.
    //
    // The window is a CAS away from a memcpy, so the two publishers have to be
    // released together: threads started per round land microseconds apart and
    // never see it. They are started once and run off a spin barrier instead,
    // which puts both inside publish_one() within a cache-line transfer of each
    // other, and one fresh table per round so each race starts from empty.
    constexpr int kRounds = 4000;

    std::vector<std::unique_ptr<ControlBlockOwner>> blocks;
    blocks.reserve(kRounds);
    for (int r = 0; r < kRounds; ++r) {
        blocks.push_back(std::make_unique<ControlBlockOwner>());
        blocks.back()->create_local(8);
    }

    std::atomic<int>      round{-1};
    std::atomic<int>      done{0};
    std::vector<uint32_t> conflicts[2] = {std::vector<uint32_t>(kRounds, 0),
                                          std::vector<uint32_t>(kRounds, 0)};

    auto worker = [&](name_fn fn, std::vector<uint32_t>& out) {
        for (int r = 0; r < kRounds; ++r) {
            while (round.load(std::memory_order_acquire) != r) {
                // A spin, not a yield: a sleep here is the whole window.
            }
            out[static_cast<size_t>(r)] = blocks[static_cast<size_t>(r)]->publish_names(1, fn);
            done.fetch_add(1, std::memory_order_acq_rel);
        }
    };

    std::thread a(worker, &one_name_alpha, std::ref(conflicts[0]));
    std::thread b(worker, &one_name_beta, std::ref(conflicts[1]));
    for (int r = 0; r < kRounds; ++r) {
        done.store(0, std::memory_order_release);
        round.store(r, std::memory_order_release);
        while (done.load(std::memory_order_acquire) != 2) {
        }
    }
    a.join();
    b.join();

    for (int r = 0; r < kRounds; ++r) {
        const size_t i = static_cast<size_t>(r);
        ASSERT_EQ(blocks[i]->claimed_names(), 1u)
            << "round " << r << ": one key must not occupy two slots";
        ASSERT_EQ(conflicts[0][i] + conflicts[1][i], 1u)
            << "round " << r << ": the loser must report the disagreement";
        const std::string_view got = blocks[i]->lookup(0, kBeginStep);
        ASSERT_TRUE(got == "alpha" || got == "beta") << "round " << r;
    }
}
