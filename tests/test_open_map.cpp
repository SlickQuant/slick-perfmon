// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The open-addressing map carries both the open-span table and the stage
// table, and its backward-shift deletion is the trickiest code in the backend:
// a bug there would silently lose spans rather than crash.

#include <slick/perfmon/detail/open_map.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <vector>

using slick::perfmon::detail::next_pow2;
using slick::perfmon::detail::open_map;

TEST(OpenMap, NextPow2) {
    EXPECT_EQ(next_pow2(0), 2u);
    EXPECT_EQ(next_pow2(1), 2u);
    EXPECT_EQ(next_pow2(2), 2u);
    EXPECT_EQ(next_pow2(3), 4u);
    EXPECT_EQ(next_pow2(1024), 1024u);
    EXPECT_EQ(next_pow2(1025), 2048u);
}

TEST(OpenMap, InsertFindErase) {
    open_map<int> m(16);
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.find(1), nullptr);

    ASSERT_NE(m.insert(1, 100), nullptr);
    ASSERT_NE(m.insert(2, 200), nullptr);
    EXPECT_EQ(m.size(), 2u);
    ASSERT_NE(m.find(1), nullptr);
    EXPECT_EQ(*m.find(1), 100);
    EXPECT_EQ(*m.find(2), 200);

    EXPECT_TRUE(m.erase(1));
    EXPECT_EQ(m.find(1), nullptr);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_FALSE(m.erase(1));
    ASSERT_NE(m.find(2), nullptr) << "erasing one key must not disturb another";
}

TEST(OpenMap, InsertingAnExistingKeyReturnsTheExistingValue) {
    open_map<int> m(8);
    int*          first = m.insert(7, 1);
    ASSERT_NE(first, nullptr);
    int* again = m.insert(7, 999);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(first, again);
    EXPECT_EQ(*again, 1) << "insert must not overwrite";
    EXPECT_EQ(m.size(), 1u);
}

TEST(OpenMap, RefusesToGrowPastItsCap) {
    // A measurement tool that allocates under load distorts what it measures,
    // so the honest failure is a null return that the caller counts.
    open_map<int> m(4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(m.insert(static_cast<uint64_t>(i), i), nullptr);
    }
    EXPECT_TRUE(m.full());
    EXPECT_EQ(m.insert(99, 99), nullptr);
    // An existing key must still resolve even when full.
    ASSERT_NE(m.find(2), nullptr);
    EXPECT_NE(m.insert(2, 0), nullptr);
}

TEST(OpenMap, SurvivesKeysThatCollideInTheLowBits) {
    // Real keys are packed bitfields - (point << 32) | seq - so without hashing
    // every point would land on one slot. These keys are identical mod any
    // power of two below 2^32.
    open_map<uint64_t> m(256);
    std::vector<uint64_t> keys;
    for (uint64_t i = 0; i < 200; ++i) {
        keys.push_back(i << 32);
    }
    for (uint64_t k : keys) {
        ASSERT_NE(m.insert(k, k), nullptr) << "key " << k;
    }
    for (uint64_t k : keys) {
        ASSERT_NE(m.find(k), nullptr) << "key " << k;
        EXPECT_EQ(*m.find(k), k);
    }
}

TEST(OpenMap, BackwardShiftDeletionKeepsEveryOtherKeyReachable) {
    // The failure mode this guards against is subtle: a careless erase breaks
    // the probe chain, and unrelated keys silently stop being found.
    constexpr uint32_t kCap = 512;
    open_map<uint64_t> m(kCap);
    std::map<uint64_t, uint64_t> mirror;
    std::mt19937_64              rng(1234);

    for (uint64_t i = 0; i < kCap; ++i) {
        const uint64_t k = (i << 32) | (i * 7919u);
        ASSERT_NE(m.insert(k, i), nullptr);
        mirror[k] = i;
    }

    std::vector<uint64_t> order;
    for (const auto& kv : mirror) {
        order.push_back(kv.first);
    }
    std::shuffle(order.begin(), order.end(), rng);

    size_t erased = 0;
    for (uint64_t k : order) {
        ASSERT_TRUE(m.erase(k)) << "failed to erase " << k;
        mirror.erase(k);
        ++erased;

        // After every single removal, every survivor must still be reachable.
        if (erased % 37 == 0 || erased == order.size()) {
            EXPECT_EQ(m.size(), mirror.size());
            for (const auto& kv : mirror) {
                ASSERT_NE(m.find(kv.first), nullptr)
                    << "key " << kv.first << " lost after " << erased << " erases";
                EXPECT_EQ(*m.find(kv.first), kv.second);
            }
        }
    }
    EXPECT_EQ(m.size(), 0u);
}

TEST(OpenMap, InterleavedInsertAndEraseMatchesAReferenceMap) {
    open_map<uint64_t>           m(256);
    std::map<uint64_t, uint64_t> mirror;
    std::mt19937_64              rng(99);
    std::uniform_int_distribution<uint64_t> key_dist(0, 400);

    for (int step = 0; step < 50000; ++step) {
        const uint64_t k = key_dist(rng) << 32;
        if (rng() % 2 == 0) {
            if (mirror.size() < 256 || mirror.count(k)) {
                if (m.insert(k, k + 1) != nullptr) {
                    mirror.emplace(k, k + 1);
                }
            }
        } else {
            EXPECT_EQ(m.erase(k), mirror.erase(k) > 0);
        }
    }

    EXPECT_EQ(m.size(), mirror.size());
    for (const auto& kv : mirror) {
        ASSERT_NE(m.find(kv.first), nullptr) << "missing " << kv.first;
        EXPECT_EQ(*m.find(kv.first), kv.second);
    }
}

TEST(OpenMap, ForEachVisitsEveryLiveEntryExactlyOnce) {
    open_map<uint64_t> m(64);
    std::set<uint64_t> inserted;
    for (uint64_t i = 0; i < 40; ++i) {
        const uint64_t k = (i << 32) | i;
        m.insert(k, i);
        inserted.insert(k);
    }

    std::set<uint64_t> seen;
    size_t             visits = 0;
    m.for_each([&](uint64_t k, uint64_t&) {
        seen.insert(k);
        ++visits;
    });
    EXPECT_EQ(visits, inserted.size());
    EXPECT_EQ(seen, inserted);
}

TEST(OpenMap, EraseIfRemovesExactlyTheMatchingEntries) {
    open_map<uint64_t> m(128);
    for (uint64_t i = 0; i < 100; ++i) {
        m.insert(i << 32, i);
    }

    const size_t removed = m.erase_if([](uint64_t, uint64_t& v) { return v % 3 == 0; });
    EXPECT_EQ(removed, 34u);  // 0, 3, ... 99
    EXPECT_EQ(m.size(), 66u);

    for (uint64_t i = 0; i < 100; ++i) {
        uint64_t* v = m.find(i << 32);
        if (i % 3 == 0) {
            EXPECT_EQ(v, nullptr) << "value " << i << " should have been erased";
        } else {
            ASSERT_NE(v, nullptr) << "value " << i << " should have survived";
            EXPECT_EQ(*v, i);
        }
    }
}

TEST(OpenMap, ResetClearsEverything) {
    open_map<int> m(16);
    m.insert(1, 1);
    m.insert(2, 2);
    m.reset(8);
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.find(1), nullptr);
    EXPECT_NE(m.insert(1, 5), nullptr);
}

TEST(OpenMap, EraseIfScansTheTableOnceRatherThanRestarting) {
    // Regression: erase_if() used to restart its scan from slot 0 after every
    // removal, so expiring entries cost O(removals * capacity). A producer that
    // died holding thousands of open spans then turned one sweep into millions
    // of probes inside the collector, while it was meant to be draining.
    //
    // The predicate call count is the observable, and it only exposes the fault
    // when some entries survive: with every entry matching, the restarting scan
    // found its victim in the first occupied slot each time and the waste was
    // all in the slots it skipped. Removing half is the shape that shows it -
    // the restarting version walked every survivor again per removal, which for
    // this input was over two million calls against the 4096 below.
    constexpr uint32_t kEntries = 4096;
    open_map<uint64_t> m(kEntries);
    for (uint64_t i = 0; i < kEntries; ++i) {
        ASSERT_NE(m.insert(i << 32, i), nullptr);
    }

    size_t       calls   = 0;
    const size_t removed = m.erase_if([&](uint64_t, uint64_t& v) {
        ++calls;
        return v % 2 == 0;
    });

    EXPECT_EQ(removed, kEntries / 2);
    EXPECT_EQ(m.size(), kEntries / 2);
    EXPECT_LE(calls, static_cast<size_t>(kEntries) * 4)
        << "erase_if is visiting entries more than a bounded number of times";

    for (uint64_t i = 0; i < kEntries; ++i) {
        uint64_t* v = m.find(i << 32);
        if (i % 2 == 0) {
            EXPECT_EQ(v, nullptr) << "value " << i << " should have been erased";
        } else {
            ASSERT_NE(v, nullptr) << "value " << i << " should have survived";
            EXPECT_EQ(*v, i);
        }
    }
}

TEST(OpenMap, EraseIfKeepsEverySurvivorReachableAfterAMassDeletion) {
    // The single-pass scan relies on backward-shift deletion only ever moving
    // an entry *backwards* along the probe order from the slot being cleared.
    // If that reasoning were wrong, survivors would silently stop being found -
    // the same failure mode BackwardShiftDeletionKeepsEveryOtherKeyReachable
    // guards for erase(), provoked the way a sweep provokes it.
    constexpr uint32_t kCap = 1024;
    open_map<uint64_t> m(kCap);
    std::map<uint64_t, uint64_t> mirror;

    for (uint64_t i = 0; i < kCap; ++i) {
        // Keys that share their low 32 bits, so the table is one long run of
        // probe chains rather than a scatter.
        const uint64_t k = (i << 32) | 0xABCDu;
        ASSERT_NE(m.insert(k, i), nullptr);
        mirror[k] = i;
    }

    const size_t removed = m.erase_if([](uint64_t, uint64_t& v) { return v % 10 != 0; });

    for (auto it = mirror.begin(); it != mirror.end();) {
        it = it->second % 10 != 0 ? mirror.erase(it) : std::next(it);
    }

    EXPECT_EQ(removed, kCap - mirror.size());
    EXPECT_EQ(m.size(), mirror.size());
    for (const auto& kv : mirror) {
        ASSERT_NE(m.find(kv.first), nullptr) << "survivor " << kv.first << " became unreachable";
        EXPECT_EQ(*m.find(kv.first), kv.second);
    }
    for (uint64_t i = 0; i < kCap; ++i) {
        if (i % 10 != 0) {
            EXPECT_EQ(m.find((i << 32) | 0xABCDu), nullptr) << "value " << i << " survived";
        }
    }
}

TEST(OpenMap, EraseIfLeavesTheTableUsableAfterRemovingEverything) {
    open_map<uint64_t> m(64);
    for (uint64_t i = 0; i < 64; ++i) {
        ASSERT_NE(m.insert(i << 32, i), nullptr);
    }
    EXPECT_EQ(m.erase_if([](uint64_t, uint64_t&) { return true; }), 64u);
    EXPECT_EQ(m.size(), 0u);
    EXPECT_FALSE(m.full());

    for (uint64_t i = 0; i < 64; ++i) {
        ASSERT_NE(m.insert(i << 32, i + 1), nullptr) << "refill failed at " << i;
    }
    EXPECT_EQ(m.size(), 64u);
    for (uint64_t i = 0; i < 64; ++i) {
        ASSERT_NE(m.find(i << 32), nullptr);
        EXPECT_EQ(*m.find(i << 32), i + 1);
    }
}

TEST(OpenMap, ClearEmptiesTheTableWithoutResizingIt) {
    open_map<uint64_t> m(32);
    for (uint64_t i = 0; i < 32; ++i) {
        m.insert(i << 32, i);
    }
    EXPECT_TRUE(m.full());

    m.clear();
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.capacity(), 32u) << "clear() must keep the configured capacity";
    for (uint64_t i = 0; i < 32; ++i) {
        EXPECT_EQ(m.find(i << 32), nullptr);
    }
    // And it is still the same table, not a fresh one that has to reallocate.
    for (uint64_t i = 0; i < 32; ++i) {
        ASSERT_NE(m.insert(i << 32, i), nullptr);
    }
    EXPECT_EQ(m.size(), 32u);
}
