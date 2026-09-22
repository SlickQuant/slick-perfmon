// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#include <slick/perfmon/accumulator.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using namespace slick::perfmon;

namespace {

struct Reference {
    uint64_t count = 0;
    uint64_t min   = 0;
    uint64_t max   = 0;
    double   mean  = 0.0;
    double   stddev = 0.0;
};

/// Deliberately the naive two-pass form: it is numerically fine over a
/// moderate range and is what Welford has to agree with.
Reference reference_stats(const std::vector<uint64_t>& v) {
    Reference r;
    if (v.empty()) {
        return r;
    }
    r.count = v.size();
    r.min   = v.front();
    r.max   = v.front();
    double sum = 0.0;
    for (uint64_t x : v) {
        r.min = std::min(r.min, x);
        r.max = std::max(r.max, x);
        sum += static_cast<double>(x);
    }
    r.mean = sum / static_cast<double>(v.size());

    if (v.size() >= 2) {
        double ss = 0.0;
        for (uint64_t x : v) {
            const double d = static_cast<double>(x) - r.mean;
            ss += d * d;
        }
        r.stddev = std::sqrt(ss / static_cast<double>(v.size() - 1));
    }
    return r;
}

}  // namespace

TEST(Accumulator, EmptyReportsZeroesRatherThanSentinels) {
    Accumulator a;
    EXPECT_EQ(a.count(), 0u);
    // min starts at UINT64_MAX internally; leaking that into a report would be
    // far worse than showing a zero next to count = 0.
    EXPECT_EQ(a.min(), 0u);
    EXPECT_EQ(a.max(), 0u);
    EXPECT_DOUBLE_EQ(a.mean(), 0.0);
    EXPECT_DOUBLE_EQ(a.stddev(), 0.0);
}

TEST(Accumulator, SingleSampleHasNoStandardDeviation) {
    Accumulator a;
    a.add(42);
    EXPECT_EQ(a.count(), 1u);
    EXPECT_EQ(a.min(), 42u);
    EXPECT_EQ(a.max(), 42u);
    EXPECT_DOUBLE_EQ(a.mean(), 42.0);
    // Undefined mathematically; 0 is the honest thing to put in a CSV cell.
    EXPECT_DOUBLE_EQ(a.stddev(), 0.0);
}

TEST(Accumulator, MatchesATwoPassReference) {
    std::mt19937_64                         rng(2024);
    std::uniform_int_distribution<uint64_t> dist(50, 5000);
    std::vector<uint64_t>                   values;
    Accumulator                             a;

    for (int i = 0; i < 100000; ++i) {
        const uint64_t v = dist(rng);
        values.push_back(v);
        a.add(v);
    }

    const Reference r = reference_stats(values);
    EXPECT_EQ(a.count(), r.count);
    EXPECT_EQ(a.min(), r.min);
    EXPECT_EQ(a.max(), r.max);
    EXPECT_NEAR(a.mean(), r.mean, r.mean * 1e-9);
    EXPECT_NEAR(a.stddev(), r.stddev, r.stddev * 1e-6);
}

TEST(Accumulator, StaysAccurateWithALargeOffset) {
    // The reason Welford is here rather than a sum of squares: with values
    // clustered far from zero, the naive form computes the variance as a small
    // difference between two enormous numbers and loses most of its digits.
    std::mt19937_64                         rng(77);
    std::uniform_int_distribution<uint64_t> jitter(0, 200);
    constexpr uint64_t                      kOffset = 3'000'000'000ull;

    std::vector<uint64_t> values;
    Accumulator           a;
    for (int i = 0; i < 50000; ++i) {
        const uint64_t v = kOffset + jitter(rng);
        values.push_back(v);
        a.add(v);
    }

    const Reference r = reference_stats(values);
    EXPECT_NEAR(a.mean(), r.mean, 1e-3);
    EXPECT_NEAR(a.stddev(), r.stddev, r.stddev * 1e-6);
    EXPECT_GT(a.stddev(), 1.0) << "a collapsed stddev would mean cancellation ate the result";
}

TEST(Accumulator, MergeEqualsAccumulatingEverythingAtOnce) {
    std::mt19937_64                         rng(31337);
    std::uniform_int_distribution<uint64_t> dist(1, 100000);

    Accumulator           part_a, part_b, whole;
    std::vector<uint64_t> values;
    for (int i = 0; i < 40000; ++i) {
        const uint64_t v = dist(rng);
        values.push_back(v);
        (i % 3 == 0 ? part_a : part_b).add(v);
        whole.add(v);
    }

    Accumulator merged;
    part_a.merge_into(merged);
    part_b.merge_into(merged);

    EXPECT_EQ(merged.count(), whole.count());
    EXPECT_EQ(merged.min(), whole.min());
    EXPECT_EQ(merged.max(), whole.max());
    EXPECT_NEAR(merged.mean(), whole.mean(), whole.mean() * 1e-9);
    // Chan's parallel variance is exact up to floating point, not an estimate.
    EXPECT_NEAR(merged.stddev(), whole.stddev(), whole.stddev() * 1e-6);
}

TEST(Accumulator, MergingAnEmptyAccumulatorChangesNothing) {
    Accumulator src, dst;
    for (uint64_t v = 1; v <= 100; ++v) {
        dst.add(v);
    }
    const double mean_before   = dst.mean();
    const double stddev_before = dst.stddev();

    src.merge_into(dst);

    EXPECT_EQ(dst.count(), 100u);
    EXPECT_DOUBLE_EQ(dst.mean(), mean_before);
    EXPECT_DOUBLE_EQ(dst.stddev(), stddev_before);
}

TEST(Accumulator, MergingIntoAnEmptyAccumulatorCopiesTheSource) {
    Accumulator src, dst;
    for (uint64_t v = 10; v <= 20; ++v) {
        src.add(v);
    }
    src.merge_into(dst);

    EXPECT_EQ(dst.count(), src.count());
    EXPECT_EQ(dst.min(), 10u);
    EXPECT_EQ(dst.max(), 20u);
    EXPECT_NEAR(dst.mean(), src.mean(), 1e-12);
    EXPECT_NEAR(dst.stddev(), src.stddev(), 1e-12);
}

TEST(Accumulator, ClearRestoresTheEmptyState) {
    Accumulator a;
    for (uint64_t v = 1; v <= 1000; ++v) {
        a.add(v);
    }
    a.clear();
    EXPECT_EQ(a.count(), 0u);
    EXPECT_EQ(a.min(), 0u);
    EXPECT_EQ(a.max(), 0u);
    EXPECT_DOUBLE_EQ(a.mean(), 0.0);
    EXPECT_DOUBLE_EQ(a.stddev(), 0.0);
    EXPECT_EQ(a.histogram().total(), 0u);
}

TEST(Accumulator, FeedsItsHistogram) {
    Accumulator a;
    for (uint64_t v = 100; v < 1100; ++v) {
        a.add(v);
    }
    EXPECT_EQ(a.histogram().total(), 1000u);
    EXPECT_GT(a.percentile(50.0), 500.0);
    EXPECT_LT(a.percentile(50.0), 700.0);
}

TEST(Accumulator, PercentilesNeverEscapeTheObservedRange) {
    // A bucket midpoint can sit above every sample that landed in that bucket,
    // so without clamping a report can show p99.9 above max - which reads as a
    // bug in the tool even when the estimate is inside its stated error.
    std::mt19937_64                     rng(555);
    std::lognormal_distribution<double> dist(7.0, 1.5);
    Accumulator                         a;
    for (int i = 0; i < 20000; ++i) {
        a.add(static_cast<uint64_t>(dist(rng)) + 1);
    }

    for (double p : {0.0, 1.0, 50.0, 90.0, 99.0, 99.9, 100.0}) {
        const double v = a.percentile(p);
        EXPECT_GE(v, static_cast<double>(a.min())) << "p" << p << " fell below min";
        EXPECT_LE(v, static_cast<double>(a.max())) << "p" << p << " rose above max";
    }
}

TEST(Accumulator, SingleSamplePercentileIsThatSample) {
    Accumulator a;
    a.add(1234);
    EXPECT_DOUBLE_EQ(a.percentile(50.0), 1234.0);
    EXPECT_DOUBLE_EQ(a.percentile(99.9), 1234.0);
}
